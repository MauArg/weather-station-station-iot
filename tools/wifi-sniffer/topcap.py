"""Converts the sniffer's `#P` output into a .pcap file for Wireshark.

    python topcap.py capture.log output.pcap

Each line from the sniffer in pcap mode is:

    #P <microseconds> <rssi_dbm> <original_length> <frame_in_base64>

`original_length` can be larger than the decoded frame if it was truncated
at PCAP_SNAPLEN; the pcap format distinguishes this (incl_len vs orig_len)
and Wireshark flags it as "packet size limited during capture".

Written with DLT_IEEE802_11 (105), i.e. raw 802.11 with no radiotap. That's
why RSSI doesn't make it into the pcap — but it's in the sniffer's text
output, and here the only thing that matters is the packet contents.

─── NOTE: frames carry an FCS ────────────────────────────────────────────────

The `sig_len` the ESP32's promiscuous mode delivers **includes the 4 FCS
bytes** at the end of every frame (verified: an ACK comes out to 14 B = 10
header + 4). If Wireshark doesn't know that, it interprets those 4 bytes as
part of the body and flags the frames as malformed.

    Preferences → Protocols → IEEE 802.11 → ✓ "Assume packets have FCS"

─── To decrypt in Wireshark ───────────────────────────────────────────────────

    Edit → Preferences → Protocols → IEEE 802.11
      ✓ Enable decryption
      Decryption keys → new key of type `wpa-pwd`, with the value:

          <network_password>:Ire y Mau IoT

The PSK doesn't appear in any file in this repo: it's typed directly into
Wireshark. And it works because the node redoes the 4-way handshake on
every cycle, which is what Wireshark needs to derive that session's PTK:
without that handshake captured there's no way to decrypt that
association's traffic.
"""
import base64
import struct
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "sniffer-pcap.log"
dst = sys.argv[2] if len(sys.argv) > 2 else "captura.pcap"

DLT_IEEE802_11 = 105
SNAPLEN = 65535

pkts = []
bad = 0
for line in open(src, encoding="utf-8-sig", errors="replace"):
    i = line.find("#P ")
    if i < 0:
        continue
    parts = line[i + 3:].split()
    if len(parts) < 4:
        bad += 1
        continue
    try:
        us, rssi, orig = int(parts[0]), int(parts[1]), int(parts[2])
        raw = base64.b64decode(parts[3], validate=True)
    except (ValueError, Exception):
        bad += 1
        continue
    pkts.append((us, rssi, orig, raw))

if not pkts:
    print(f"no #P lines found in {src}")
    sys.exit(1)

# The sniffer only has micros() since its own boot, not wall-clock time. The
# first packet is anchored at t=0 and the deltas are kept, which is what
# matters for reading a sequence. Wireshark shows relative times either way.
base_us = pkts[0][0]

with open(dst, "wb") as f:
    f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, SNAPLEN, DLT_IEEE802_11))
    for us, _rssi, orig, raw in pkts:
        d = us - base_us
        f.write(struct.pack("<IIII", d // 1_000_000, d % 1_000_000, len(raw), max(orig, len(raw))))
        f.write(raw)

trunc = sum(1 for _, _, orig, raw in pkts if orig > len(raw))
span = (pkts[-1][0] - pkts[0][0]) / 1e6
print(f"{len(pkts)} frames -> {dst}")
print(f"  window: {span:.1f} s")
print(f"  truncated: {trunc}" + ("  (won't be decryptable)" if trunc else ""))
if bad:
    print(f"  lines dropped for being cut off: {bad}")
print()
print("In Wireshark: Preferences → Protocols → IEEE 802.11 → Enable decryption,")
print("and add a `wpa-pwd` key with  <password>:Ire y Mau IoT")
