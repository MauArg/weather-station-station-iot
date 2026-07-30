"""Convierte la salida `#P` del sniffer en un archivo .pcap para Wireshark.

    python topcap.py captura.log salida.pcap

Cada línea del sniffer en modo pcap es:

    #P <microsegundos> <rssi_dbm> <largo_original> <trama_en_base64>

El `largo_original` puede ser mayor que la trama decodificada si se truncó en
PCAP_SNAPLEN; el formato pcap lo distingue (incl_len vs orig_len) y Wireshark lo
marca como "packet size limited during capture".

Se escribe con DLT_IEEE802_11 (105), o sea 802.11 crudo sin radiotap. El RSSI no
entra en el pcap por eso — pero está en la salida de texto del sniffer, y acá lo
único que interesa es el contenido de los paquetes.

─── Para desencriptar en Wireshark ───────────────────────────────────────────

    Editar → Preferencias → Protocols → IEEE 802.11
      ✓ Enable decryption
      Decryption keys → nueva clave de tipo `wpa-pwd`, con el valor:

          <password_de_la_red>:Ire y Mau IoT

El PSK no aparece en ningún archivo de este repo: se escribe directo en
Wireshark. Y funciona porque el nodo rehace el handshake de 4 vías en cada
ciclo, que es lo que Wireshark necesita para derivar la PTK de esa sesión: sin
ese handshake capturado no hay forma de descifrar el tráfico de esa asociación.
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
    print(f"no se encontró ninguna línea #P en {src}")
    sys.exit(1)

# El sniffer sólo tiene micros() desde su arranque, no hora de pared. Se ancla el
# primer paquete en t=0 y se conservan los deltas, que es lo que importa para
# leer una secuencia. Wireshark muestra tiempos relativos igual.
base_us = pkts[0][0]

with open(dst, "wb") as f:
    f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, SNAPLEN, DLT_IEEE802_11))
    for us, _rssi, orig, raw in pkts:
        d = us - base_us
        f.write(struct.pack("<IIII", d // 1_000_000, d % 1_000_000, len(raw), max(orig, len(raw))))
        f.write(raw)

trunc = sum(1 for _, _, orig, raw in pkts if orig > len(raw))
span = (pkts[-1][0] - pkts[0][0]) / 1e6
print(f"{len(pkts)} tramas -> {dst}")
print(f"  ventana: {span:.1f} s")
print(f"  truncadas: {trunc}" + ("  (no se van a poder desencriptar)" if trunc else ""))
if bad:
    print(f"  líneas descartadas por estar cortadas: {bad}")
print()
print("En Wireshark: Preferencias → Protocols → IEEE 802.11 → Enable decryption,")
print("y agregar una clave `wpa-pwd` con  <password>:Ire y Mau IoT")
