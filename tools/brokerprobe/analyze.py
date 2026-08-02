"""Analyzes brokerprobe's NDJSON.

The fine-grained question: in a lost cycle, did the broker receive the
CONNECT and only miss the PUBLISH, or did it receive nothing at all?

$SYS/broker/bytes/received answers this better than messages/received,
because the noise from other clients is 2 B PINGREQs while one node cycle
contributes ~600 B:

    CONNECT     ~70 B  (client id 18 + user 19 + pass 12 + fixed)
    SUBSCRIBE   ~24 B
    PUBLISH     ~503 B (478 payload + topic 20 + header)
    DISCONNECT     2 B

And TCP delivers in order: the broker can't process the DISCONNECT without
having already received the PUBLISH bytes. So ~94 B in a lost cycle means
CONNECT+SUBSCRIBE and nothing else — the connection was alive and the
uplink died afterward.
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
            continue                      # last line, partial write
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
    """Counter (t, delta) transitions."""
    pts = series.get(key, [])
    return [(pts[i][0], pts[i][1] - pts[i - 1][1]) for i in range(1, len(pts))]


t0 = min([t for t, _, _ in telemetry] + [p[0] for pts in series.values() for p in pts])
t1 = max([t for t, _, _ in telemetry] + [p[0] for pts in series.values() for p in pts])


def rel(t):
    return t.strftime("%H:%M:%S")


print("=" * 76)
print(f"Window {rel(t0)} → {rel(t1)}  ({(t1 - t0).total_seconds() / 60:.1f} min)")
print("=" * 76)

boots = [b for _, b, _ in telemetry if b is not None]
if boots:
    lo, hi = min(boots), max(boots)
    lived = hi - lo + 1
    missing = sorted(set(range(lo, hi + 1)) - set(boots))
    print(f"\nboot_count {lo}..{hi}: {lived} cycles lived, {len(boots)} delivered")
    print(f"Lost: {len(missing)} = {100 * len(missing) / lived:.0f}%   -> {missing}")
    seq = "".join("." if b in boots else "X" for b in range(lo, hi + 1))
    print(f"Sequence (. arrived, X lost): {seq}")

pub = deltas("publish/messages/received")
pubbytes = deltas("publish/bytes/received")
print(f"\nPUBLISH ingested by the broker: {sum(d for _, d in pub)}"
      f"   ({sum(d for _, d in pubbytes)} B)")
print(f"Telemetry delivered to this subscriber: {len(telemetry)}")

# ── The fine-grained part: byte ingress around each lost cycle ──────────────
byt = deltas("bytes/received")
print("\n" + "─" * 76)
print("BYTE INGRESS PER SAMPLE ($SYS every ~10 s)")
print("─" * 76)
print("Reference: a delivered cycle ≈ 600 B. CONNECT+SUBSCRIBE only ≈ 94 B.")
print("PINGREQ from other clients = 2 B each.\n")

# Expected moment of each wake: interpolated from the delivered ones, which
# carry boot_count, assuming the cadence measured between consecutive
# deliveries.
known = {b: t for t, b, _ in telemetry if b is not None}
if len(known) >= 2:
    bs = sorted(known)
    period = (known[bs[-1]] - known[bs[0]]).total_seconds() / (bs[-1] - bs[0])
    print(f"Measured cadence: {period:.1f} s per cycle\n")

    anchor_b, anchor_t = bs[0], known[bs[0]]
    rows = []
    for b in range(bs[0], bs[-1] + 1):
        if b in known:
            expect, kind = known[b], "arrived"
        else:
            expect, kind = datetime.fromtimestamp(
                anchor_t.timestamp() + (b - anchor_b) * period,
                tz=anchor_t.tzinfo), "LOST"
        # $SYS sample that covers that instant (the first one after it)
        cover = next(((t, d) for t, d in byt if t >= expect), None)
        nearby = sum(d for t, d in byt if abs((t - expect).total_seconds()) <= 12)
        rows.append((b, kind, expect, cover[1] if cover else None, nearby))

    # Exact sizes of the node's packets, to read the deltas without estimating.
    #   CONNECT     67 B = 2 + (2+4 "MQTT") + 1 level + 1 flags + 2 keepalive
    #                        + (2+18 client id) + (2+19 user) + (2+12 pass)
    #   SUBSCRIBE   21 B = 2 + 2 packet id + (2+14 "station/01/cmd") + 1 qos
    #   PUBLISH    503 B = 3 + (2+20 "station/01/telemetry") + 478 payload
    #   DISCONNECT   2 B
    # A complete cycle is 593 B. The noise is the other clients' PINGREQs
    # (2 B) plus the container healthcheck, which connects and leaves every
    # 30 s (~75 B).
    pkts = deltas("messages/received")
    print(f"{'boot':>6}  {'state':<8} {'wake':>9}  {'sample':>9}  {'pkts':>5}  {'±12 s':>9}")
    for b, kind, expect, cov, nearby in rows:
        mark = "  <<<" if kind == "LOST" else ""
        p = next((d for t, d in pkts if t >= expect), None)
        print(f"{b:>6}  {kind:<8} {rel(expect):>9}  "
              f"{(str(cov) + ' B') if cov is not None else '—':>9}  "
              f"{p if p is not None else '—':>5}  {str(nearby) + ' B':>9}{mark}")

    lost = [n for _, k, _, _, n in rows if k == "LOST"]
    ok = [n for _, k, _, _, n in rows if k == "arrived"]
    if lost and ok:
        print(f"\nMedian bytes ±12 s from the wake:")
        print(f"   cycles that arrived : {sorted(ok)[len(ok) // 2]} B")
        print(f"   cycles lost         : {sorted(lost)[len(lost) // 2]} B")
        print("\nReading:")
        print("  ~600 B in a lost one  -> impossible: the DISCONNECT can't arrive")
        print("                           without the PUBLISH (TCP delivers in order)")
        print("  ~94 B  in a lost one  -> CONNECT and SUBSCRIBE arrived, the PUBLISH")
        print("                           didn't: the uplink died AFTER the handshake")
        print("  ~0 B   in a lost one  -> the broker saw nothing, which contradicts")
        print("                           the node's LOG_MQTT_OK (it received the CONNACK)")

conn = series.get("clients/connected", [])
if conn:
    print(f"\nclients/connected: {len(conn)} transitions, range "
          f"{min(v for _, v in conn)}..{max(v for _, v in conn)} "
          "(noisy: browsers come and go)")
