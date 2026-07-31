"""Cruza el sniffer 802.11 con las llegadas de telemetría al broker.

La prueba de la hipótesis del enlace asimétrico: si la pérdida es falta de margen
de subida, los ciclos perdidos tienen que mostrar peor RSSI del nodo medido EN EL
AP y/o más reintentos que los que llegan. Si salen indistinguibles, el margen es
malo siempre y lo que decide un ciclo es otra cosa.

Uso: python crossref.py sniffer-long.log longrun.ndjson
"""
import json
import re
import statistics
import sys
from datetime import datetime, timedelta

sniff_path = sys.argv[1] if len(sys.argv) > 1 else "sniffer-long.log"
probe_path = sys.argv[2] if len(sys.argv) > 2 else "longrun.ndjson"

NODE = "80f1b26df9fc"

# ── Tramas del sniffer ───────────────────────────────────────────────────────
# La salida trae cajas Unicode que el serial mastica, así que se ancla en lo que
# es estable: hora del host, millis del sniffer, RSSI y el resto de la línea.
LINE = re.compile(r"^(\d{2}:\d{2}:\d{2}\.\d{3})\s+(\d+)\s+(-?\d+)\s+(\S+)(.*)$")

frames = []
for raw in open(sniff_path, encoding="utf-8-sig", errors="replace"):
    m = LINE.match(raw.strip())
    if not m:
        continue
    rest = m.group(5)
    a2 = re.search(r"a2=(\S+)", rest)
    frames.append({
        "host": datetime.strptime(m.group(1), "%H:%M:%S.%f"),
        "t": int(m.group(2)),
        "rssi": int(m.group(3)),
        "kind": m.group(4),
        "retry": "RETRY" in rest,
        "from_node": bool(a2 and a2.group(1) == NODE),
        "to_node": bool(re.search(r"a1=" + NODE, rest)),
    })

if not frames:
    print("sin tramas parseadas")
    sys.exit()

# ── Ráfagas: se cortan por hueco en el millis del sniffer, que es monótono ────
# El reloj del host se corre por el buffer del serial; el millis de la placa no.
bursts = []
cur = [frames[0]]
for f in frames[1:]:
    if f["t"] - cur[-1]["t"] > 5000:
        bursts.append(cur)
        cur = [f]
    else:
        cur.append(f)
bursts.append(cur)

# Sólo las que contienen tramas del nodo: el resto son deauth broadcast de
# vecinos, que entran al filtro a propósito pero no son ciclos.
bursts = [b for b in bursts if any(f["from_node"] for f in b)]

# ── Telemetría entregada ─────────────────────────────────────────────────────
telem = []
for raw in open(probe_path, encoding="utf-8"):
    raw = raw.strip()
    if not raw:
        continue
    try:
        d = json.loads(raw)
    except json.JSONDecodeError:
        continue
    if d["topic"].endswith("/telemetry"):
        t = datetime.fromisoformat(d["t"]).replace(tzinfo=None)
        telem.append((datetime.strptime(t.strftime("%H:%M:%S.%f"), "%H:%M:%S.%f"),
                      d.get("boot_count")))

print("=" * 78)
print(f"{len(bursts)} ráfagas con tramas del nodo   |   {len(telem)} telemetrías entregadas")
print("=" * 78)

rows = []
for b in bursts:
    nf = [f for f in b if f["from_node"]]
    rssis = [f["rssi"] for f in nf]
    retries = sum(1 for f in nf if f["retry"])
    start, end = b[0]["host"], b[-1]["host"]
    hit = next((bc for tt, bc in telem
                if start - timedelta(seconds=3) <= tt <= end + timedelta(seconds=8)), None)
    rows.append({
        "start": start, "n": len(nf),
        "rssi_med": statistics.median(rssis) if rssis else None,
        "rssi_min": min(rssis) if rssis else None,
        "retry_pct": 100 * retries / len(nf) if nf else 0,
        "dur_ms": b[-1]["t"] - b[0]["t"],
        "hit": hit,
    })

ok = [r for r in rows if r["hit"] is not None]
lost = [r for r in rows if r["hit"] is None]

print(f"\nráfagas con telemetría : {len(ok)}")
print(f"ráfagas SIN telemetría : {len(lost)}")


def stat(rs, key):
    v = [r[key] for r in rs if r[key] is not None]
    return statistics.median(v) if v else float("nan")


print("\n" + "─" * 78)
print(f"{'grupo':<24} {'n':>4} {'RSSI med':>10} {'RSSI min':>10} {'% RETRY':>9} {'tramas':>8} {'dur':>8}")
print("─" * 78)
for name, rs in (("ciclos que llegaron", ok), ("ciclos PERDIDOS", lost)):
    if not rs:
        continue
    print(f"{name:<24} {len(rs):>4} {stat(rs,'rssi_med'):>10.1f} {stat(rs,'rssi_min'):>10.1f} "
          f"{stat(rs,'retry_pct'):>8.0f}% {stat(rs,'n'):>8.0f} {stat(rs,'dur_ms'):>7.0f}ms")

if ok and lost:
    dr = stat(lost, "rssi_med") - stat(ok, "rssi_med")
    dq = stat(lost, "retry_pct") - stat(ok, "retry_pct")
    print("\n" + "─" * 78)
    print("LECTURA")
    print("─" * 78)
    print(f"Diferencia de RSSI (perdidos − llegados): {dr:+.1f} dB")
    print(f"Diferencia de % de RETRY:                 {dq:+.0f} puntos")
    print()
    # El % de RETRY NO sirve para decidir esto: es casi tautológico, porque un
    # ciclo perdido es por definición uno cuyas tramas no fueron reconocidas, y
    # cada no-reconocimiento genera un reintento. La primera versión de este
    # script concluía con esa diferencia y daba un veredicto equivocado. El dato
    # no circular es el RSSI, que se mide igual pase lo que pase después.
    if dr < -2:
        print("⇒ Los ciclos perdidos llegan al AP más débiles. La pérdida sigue el")
        print("  nivel de señal, y el trabajo es de antena o de ubicación.")
    else:
        print("⇒ El RSSI es INDISTINGUIBLE entre los que llegan y los que no.")
        print("  El margen de subida es igual de malo en los dos grupos, así que")
        print("  explica el RÉGIMEN —por qué se pierde ~1 de cada 4— pero no CUÁL")
        print("  ciclo se pierde: eso lo decide que una racha de reintentos fallidos")
        print("  agote el límite. Mejorar el margen igual debería bajar la tasa, y")
        print("  de forma más que proporcional, porque cada dB reduce la")
        print("  probabilidad de fallo por trama y las rachas son potencias de eso.")
        print(f"  (El {stat(ok,'retry_pct'):.0f}% de RETRY de los ciclos SANOS es el número")
        print("  que mide el margen sin circularidad: un enlace sano está en 1-5%.)")

# Distribución completa, por si la mediana esconde una cola.
print("\nRSSI del nodo por ráfaga (mediana), en orden temporal:")
print("  llegados:  " + " ".join(f"{r['rssi_med']:.0f}" for r in ok if r["rssi_med"]))
print("  perdidos:  " + " ".join(f"{r['rssi_med']:.0f}" for r in lost if r["rssi_med"]))
