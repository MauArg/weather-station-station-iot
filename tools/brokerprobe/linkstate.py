"""Reads the pv_* fields from 1.9.0 and separates how lost cycles closed
from how delivered cycles closed.

Each telemetry payload carries a snapshot of the PREVIOUS cycle's link
(pv_boot identifies which one). Cross-referencing that against the list of
boot_count values that actually arrived yields what can't be seen from the
outside: what the node saw at the end of a cycle that published nothing.

    pv_st = 3 (WL_CONNECTED)  -> the driver believes it's associated while deaf and mute
    pv_st = 5 / 6             -> the node knew it had lost the link
"""
import json
import statistics
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "linkstate.ndjson"

WL = {0: "IDLE", 1: "NO_SSID", 3: "CONNECTED", 4: "CONNECT_FAILED",
      5: "CONNECTION_LOST", 6: "DISCONNECTED", 255: "NO_SHIELD"}
MQ = {0: "connected", -1: "disconnected", -2: "connect failed",
      -3: "connection lost", -4: "timeout"}

telem = []
for line in open(path, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        continue
    if d["topic"].endswith("/telemetry") and d.get("doc"):
        telem.append(d["doc"])

delivered = {int(t["boot_count"]) for t in telem if "boot_count" in t}
if not delivered:
    print("no telemetry yet")
    sys.exit()
lo, hi = min(delivered), max(delivered)
lived = hi - lo + 1
lost = sorted(set(range(lo, hi + 1)) - delivered)

print("=" * 74)
print(f"boot {lo}..{hi}: {lived} cycles lived, {len(delivered)} delivered, "
      f"{len(lost)} lost = {100*len(lost)/lived:.0f}%")
print("Sequence:", "".join("." if b in delivered else "X" for b in range(lo, hi + 1)))
print("=" * 74)

# Each pv_* snapshot describes cycle pv_boot, not the one carrying it.
snapshots = {}
for t in telem:
    if "pv_boot" in t:
        snapshots[int(t["pv_boot"])] = t

print(f"\nSnapshots received: {len(snapshots)} of {lived} cycles\n")
print(f"{'cycle':>6}  {'result':<10} {'pv_st':<18} {'pv_mq':<18} {'rssi':>6} {'awake':>10}")

groups = {"arrived": [], "LOST": []}
for b in sorted(snapshots):
    f = snapshots[b]
    res = "arrived" if b in delivered else "LOST"
    st, mq = int(f.get("pv_st", -1)), int(f.get("pv_mq", 99))
    groups[res].append((st, mq, f.get("pv_rssi"), f.get("pv_ms")))
    mark = "  <<<" if res == "LOST" else ""
    print(f"{b:>6}  {res:<10} {st}={WL.get(st,'?'):<15} "
          f"{mq}={MQ.get(mq,'?'):<15} {f.get('pv_rssi'):>5} {f.get('pv_ms'):>8} ms{mark}")

print("\n" + "─" * 74)
print("VERDICT")
print("─" * 74)
for res in ("arrived", "LOST"):
    g = groups[res]
    if not g:
        print(f"\n{res}: no samples yet")
        continue
    st = Counter(x[0] for x in g)
    mq = Counter(x[1] for x in g)
    ms = [x[3] for x in g if x[3] is not None]
    print(f"\nCycles that {res} (n={len(g)}):")
    print("  WiFi.status(): " + ", ".join(f"{WL.get(k,k)}={v}" for k, v in st.most_common()))
    print("  mqtt.state():  " + ", ".join(f"{MQ.get(k,k)}={v}" for k, v in mq.most_common()))
    if ms:
        print(f"  awake: median {statistics.median(ms):.0f} ms, "
              f"range {min(ms)}-{max(ms)} ms")

if groups["LOST"]:
    connected = sum(1 for x in groups["LOST"] if x[0] == 3)
    n = len(groups["LOST"])
    print()
    if connected == n:
        print(f"⇒ All {n} lost cycles closed with WL_CONNECTED: the node has NO")
        print("  idea. The driver believes it's associated while deaf and mute, so")
        print("  re-checking WiFi.status() would be useless — in-band delivery")
        print("  confirmation (echo of the topic itself) + republish is needed.")
    elif connected == 0:
        print(f"⇒ All {n} lost cycles closed WITHOUT association: the node knows")
        print("  and does nothing about it. The fix is cheap: re-check before")
        print("  publishing and re-associate. connectWiFi() today never checks")
        print("  the state again.")
    else:
        print(f"⇒ Mixed: {connected}/{n} lost cycles closed with WL_CONNECTED.")
        print("  There are two distinct failure modes and they're worth handling separately.")
