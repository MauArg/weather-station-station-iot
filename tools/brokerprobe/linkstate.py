"""Lee los campos pv_* del 1.9.0 y separa cómo cerraron los ciclos que se
perdieron de cómo cerraron los que llegaron.

Cada telemetría trae la foto del enlace del ciclo ANTERIOR (pv_boot identifica
cuál). Cruzando eso contra la lista de boot_count que efectivamente llegaron, se
obtiene lo que no se puede ver desde afuera: qué vio el nodo al final de un ciclo
que no publicó nada.

    pv_st = 3 (WL_CONNECTED)  -> el driver se cree asociado estando sordo y mudo
    pv_st = 5 / 6             -> el nodo sabía que había perdido el enlace
"""
import json
import statistics
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "linkstate.ndjson"

WL = {0: "IDLE", 1: "NO_SSID", 3: "CONNECTED", 4: "CONNECT_FAILED",
      5: "CONNECTION_LOST", 6: "DISCONNECTED", 255: "NO_SHIELD"}
MQ = {0: "conectado", -1: "desconectado", -2: "connect falló",
      -3: "conexión perdida", -4: "timeout"}

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
    print("sin telemetría todavía")
    sys.exit()
lo, hi = min(delivered), max(delivered)
lived = hi - lo + 1
lost = sorted(set(range(lo, hi + 1)) - delivered)

print("=" * 74)
print(f"boot {lo}..{hi}: {lived} ciclos vividos, {len(delivered)} entregados, "
      f"{len(lost)} perdidos = {100*len(lost)/lived:.0f}%")
print("Secuencia:", "".join("." if b in delivered else "X" for b in range(lo, hi + 1)))
print("=" * 74)

# Cada foto pv_* describe el ciclo pv_boot, no el que la transporta.
fotos = {}
for t in telem:
    if "pv_boot" in t:
        fotos[int(t["pv_boot"])] = t

print(f"\nFotos recibidas: {len(fotos)} de {lived} ciclos\n")
print(f"{'ciclo':>6}  {'resultado':<10} {'pv_st':<18} {'pv_mq':<18} {'rssi':>6} {'despierto':>10}")

grupos = {"llegó": [], "PERDIDO": []}
for b in sorted(fotos):
    f = fotos[b]
    res = "llegó" if b in delivered else "PERDIDO"
    st, mq = int(f.get("pv_st", -1)), int(f.get("pv_mq", 99))
    grupos[res].append((st, mq, f.get("pv_rssi"), f.get("pv_ms")))
    mark = "  <<<" if res == "PERDIDO" else ""
    print(f"{b:>6}  {res:<10} {st}={WL.get(st,'?'):<15} "
          f"{mq}={MQ.get(mq,'?'):<15} {f.get('pv_rssi'):>5} {f.get('pv_ms'):>8} ms{mark}")

print("\n" + "─" * 74)
print("VEREDICTO")
print("─" * 74)
for res in ("llegó", "PERDIDO"):
    g = grupos[res]
    if not g:
        print(f"\n{res}: sin muestras todavía")
        continue
    st = Counter(x[0] for x in g)
    mq = Counter(x[1] for x in g)
    ms = [x[3] for x in g if x[3] is not None]
    print(f"\nCiclos que {res} (n={len(g)}):")
    print("  WiFi.status(): " + ", ".join(f"{WL.get(k,k)}={v}" for k, v in st.most_common()))
    print("  mqtt.state():  " + ", ".join(f"{MQ.get(k,k)}={v}" for k, v in mq.most_common()))
    if ms:
        print(f"  despierto: mediana {statistics.median(ms):.0f} ms, "
              f"rango {min(ms)}-{max(ms)} ms")

if grupos["PERDIDO"]:
    conectados = sum(1 for x in grupos["PERDIDO"] if x[0] == 3)
    n = len(grupos["PERDIDO"])
    print()
    if conectados == n:
        print(f"⇒ Los {n} ciclos perdidos cerraron con WL_CONNECTED: el nodo NO se")
        print("  entera. El driver se cree asociado estando sordo y mudo, así que")
        print("  re-chequear WiFi.status() no serviría de nada — hace falta")
        print("  confirmación de entrega en banda (eco del propio topic) + republish.")
    elif conectados == 0:
        print(f"⇒ Los {n} ciclos perdidos cerraron SIN asociación: el nodo lo sabe y")
        print("  no hace nada. El arreglo es barato: re-chequear antes de publicar y")
        print("  reasociarse. connectWiFi() hoy no vuelve a mirar el estado nunca.")
    else:
        print(f"⇒ Mezcla: {conectados}/{n} perdidos cerraron con WL_CONNECTED.")
        print("  Hay dos modos de falla distintos y conviene tratarlos por separado.")
