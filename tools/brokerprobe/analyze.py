"""Analiza el NDJSON de brokerprobe.

La pregunta fina: en un ciclo perdido, ¿el broker recibió el CONNECT y le faltó
sólo el PUBLISH, o no recibió nada?

$SYS/broker/bytes/received lo contesta mejor que messages/received, porque el
ruido de los otros clientes son PINGREQ de 2 B mientras que un ciclo del nodo
aporta ~600 B:

    CONNECT     ~70 B  (client id 18 + user 19 + pass 12 + fijo)
    SUBSCRIBE   ~24 B
    PUBLISH     ~503 B (478 de payload + topic 20 + header)
    DISCONNECT     2 B

Y TCP entrega en orden: el broker no puede procesar el DISCONNECT sin haber
recibido antes los bytes del PUBLISH. Así que ~94 B en un ciclo perdido significa
CONNECT+SUBSCRIBE y nada más — la conexión estaba viva y el uplink murió después.
"""
import json
import sys
from datetime import datetime

path = sys.argv[1] if len(sys.argv) > 1 else "baseline-1.5.0.ndjson"

telemetry = []          # (t, boot_count, bytes)
series = {}             # key -> [(t, value)]

with open(path, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except json.JSONDecodeError:
            continue                      # última línea, escritura a medias
        t = datetime.fromisoformat(ev["t"])
        topic = ev["topic"]
        if topic.endswith("/telemetry"):
            telemetry.append((t, ev.get("boot_count"), ev.get("bytes", 0)))
        elif topic.startswith("$SYS/broker/"):
            key = topic[len("$SYS/broker/"):]
            try:
                val = int(str(ev.get("value", "")).split()[0])
            except (ValueError, IndexError):
                continue
            series.setdefault(key, []).append((t, val))


def deltas(key):
    """Transiciones (t, delta) del contador."""
    pts = series.get(key, [])
    return [(pts[i][0], pts[i][1] - pts[i - 1][1]) for i in range(1, len(pts))]


t0 = min([t for t, _, _ in telemetry] + [p[0] for pts in series.values() for p in pts])
t1 = max([t for t, _, _ in telemetry] + [p[0] for pts in series.values() for p in pts])


def rel(t):
    return t.strftime("%H:%M:%S")


print("=" * 76)
print(f"Ventana {rel(t0)} → {rel(t1)}  ({(t1 - t0).total_seconds() / 60:.1f} min)")
print("=" * 76)

boots = [b for _, b, _ in telemetry if b is not None]
if boots:
    lo, hi = min(boots), max(boots)
    lived = hi - lo + 1
    missing = sorted(set(range(lo, hi + 1)) - set(boots))
    print(f"\nboot_count {lo}..{hi}: {lived} ciclos vividos, {len(boots)} entregados")
    print(f"Perdidos: {len(missing)} = {100 * len(missing) / lived:.0f}%   -> {missing}")
    seq = "".join("." if b in boots else "X" for b in range(lo, hi + 1))
    print(f"Secuencia (. llegó, X perdido): {seq}")

pub = deltas("publish/messages/received")
pubbytes = deltas("publish/bytes/received")
print(f"\nPUBLISH ingresados al broker: {sum(d for _, d in pub)}"
      f"   ({sum(d for _, d in pubbytes)} B)")
print(f"Telemetría entregada a este suscriptor: {len(telemetry)}")

# ── Lo fino: bytes de ingreso alrededor de cada ciclo perdido ────────────────
byt = deltas("bytes/received")
print("\n" + "─" * 76)
print("INGRESO DE BYTES POR MUESTRA ($SYS cada ~10 s)")
print("─" * 76)
print("Referencia: un ciclo entregado ≈ 600 B. Sólo CONNECT+SUBSCRIBE ≈ 94 B.")
print("PINGREQ de otros clientes = 2 B cada uno.\n")

# Momento esperado de cada wake: se interpola desde los entregados, que traen
# boot_count, asumiendo la cadencia medida entre entregados consecutivos.
known = {b: t for t, b, _ in telemetry if b is not None}
if len(known) >= 2:
    bs = sorted(known)
    period = (known[bs[-1]] - known[bs[0]]).total_seconds() / (bs[-1] - bs[0])
    print(f"Cadencia medida: {period:.1f} s por ciclo\n")

    anchor_b, anchor_t = bs[0], known[bs[0]]
    rows = []
    for b in range(bs[0], bs[-1] + 1):
        if b in known:
            expect, kind = known[b], "llegó"
        else:
            expect, kind = datetime.fromtimestamp(
                anchor_t.timestamp() + (b - anchor_b) * period,
                tz=anchor_t.tzinfo), "PERDIDO"
        # muestra de $SYS que cubre ese instante (la primera posterior)
        cover = next(((t, d) for t, d in byt if t >= expect), None)
        nearby = sum(d for t, d in byt if abs((t - expect).total_seconds()) <= 12)
        rows.append((b, kind, expect, cover[1] if cover else None, nearby))

    # Tamaños exactos de los paquetes del nodo, para leer los deltas sin estimar.
    #   CONNECT     67 B = 2 + (2+4 "MQTT") + 1 nivel + 1 flags + 2 keepalive
    #                        + (2+18 client id) + (2+19 user) + (2+12 pass)
    #   SUBSCRIBE   21 B = 2 + 2 packet id + (2+14 "station/01/cmd") + 1 qos
    #   PUBLISH    503 B = 3 + (2+20 "station/01/telemetry") + 478 payload
    #   DISCONNECT   2 B
    # Un ciclo completo son 593 B. El ruido son los PINGREQ de los otros clientes
    # (2 B) más el healthcheck del contenedor, que conecta y sale cada 30 s (~75 B).
    pkts = deltas("messages/received")
    print(f"{'boot':>6}  {'estado':<8} {'wake':>9}  {'muestra':>9}  {'paq':>5}  {'±12 s':>9}")
    for b, kind, expect, cov, nearby in rows:
        mark = "  <<<" if kind == "PERDIDO" else ""
        p = next((d for t, d in pkts if t >= expect), None)
        print(f"{b:>6}  {kind:<8} {rel(expect):>9}  "
              f"{(str(cov) + ' B') if cov is not None else '—':>9}  "
              f"{p if p is not None else '—':>5}  {str(nearby) + ' B':>9}{mark}")

    lost = [n for _, k, _, _, n in rows if k == "PERDIDO"]
    ok = [n for _, k, _, _, n in rows if k == "llegó"]
    if lost and ok:
        print(f"\nMediana de bytes ±12 s del wake:")
        print(f"   ciclos que llegaron : {sorted(ok)[len(ok) // 2]} B")
        print(f"   ciclos perdidos     : {sorted(lost)[len(lost) // 2]} B")
        print("\nLectura:")
        print("  ~600 B en un perdido  -> imposible: el DISCONNECT no puede llegar")
        print("                           sin el PUBLISH (TCP entrega en orden)")
        print("  ~94 B  en un perdido  -> CONNECT y SUBSCRIBE llegaron, el PUBLISH")
        print("                           no: el uplink murió DESPUÉS del handshake")
        print("  ~0 B   en un perdido  -> el broker no vio nada, y eso contradice el")
        print("                           LOG_MQTT_OK del nodo (recibió el CONNACK)")

conn = series.get("clients/connected", [])
if conn:
    print(f"\nclients/connected: {len(conn)} transiciones, rango "
          f"{min(v for _, v in conn)}..{max(v for _, v in conn)} "
          "(ruidoso: hay browsers entrando y saliendo)")
