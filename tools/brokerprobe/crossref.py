"""Cross-references the 802.11 sniffer with telemetry arrivals at the broker.

The test for the asymmetric-link hypothesis: if the loss is a lack of
uplink margin, lost cycles have to show worse node RSSI as measured AT THE
AP and/or more retries than the ones that arrive. If they come out
indistinguishable, the margin is bad all the time and something else
decides a given cycle.

Usage: python crossref.py sniffer-long.log longrun.ndjson
"""
import json
import re
import statistics
import sys
from datetime import datetime, timedelta

sniff_path = sys.argv[1] if len(sys.argv) > 1 else "sniffer-long.log"
probe_path = sys.argv[2] if len(sys.argv) > 2 else "longrun.ndjson"

NODE = "80f1b26df9fc"

# ── Sniffer frames ─────────────────────────────────────────────────────────
# The output carries Unicode boxes that the serial link mangles, so this
# anchors on what's stable: host time, sniffer millis, RSSI and the rest of the line.
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
    print("no frames parsed")
    sys.exit()

# ── Bursts: split on a gap in the sniffer's millis, which is monotonic ───────
# The host clock drifts because of the serial buffer; the board's millis doesn't.
bursts = []
cur = [frames[0]]
for f in frames[1:]:
    if f["t"] - cur[-1]["t"] > 5000:
        bursts.append(cur)
        cur = [f]
    else:
        cur.append(f)
bursts.append(cur)

# Only the ones that contain frames from the node: the rest are deauth
# broadcasts from neighbors, which pass the filter on purpose but aren't cycles.
bursts = [b for b in bursts if any(f["from_node"] for f in b)]

# ── Delivered telemetry ──────────────────────────────────────────────────────
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
print(f"{len(bursts)} bursts with node frames   |   {len(telem)} telemetry deliveries")
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

print(f"\nbursts with telemetry    : {len(ok)}")
print(f"bursts WITHOUT telemetry : {len(lost)}")


def stat(rs, key):
    v = [r[key] for r in rs if r[key] is not None]
    return statistics.median(v) if v else float("nan")


print("\n" + "─" * 78)
print(f"{'group':<24} {'n':>4} {'RSSI med':>10} {'RSSI min':>10} {'% RETRY':>9} {'frames':>8} {'dur':>8}")
print("─" * 78)
for name, rs in (("cycles that arrived", ok), ("LOST cycles", lost)):
    if not rs:
        continue
    print(f"{name:<24} {len(rs):>4} {stat(rs,'rssi_med'):>10.1f} {stat(rs,'rssi_min'):>10.1f} "
          f"{stat(rs,'retry_pct'):>8.0f}% {stat(rs,'n'):>8.0f} {stat(rs,'dur_ms'):>7.0f}ms")

if ok and lost:
    dr = stat(lost, "rssi_med") - stat(ok, "rssi_med")
    dq = stat(lost, "retry_pct") - stat(ok, "retry_pct")
    print("\n" + "─" * 78)
    print("READING")
    print("─" * 78)
    print(f"RSSI difference (lost − arrived):  {dr:+.1f} dB")
    print(f"RETRY % difference:                {dq:+.0f} points")
    print()
    # RETRY % is NOT useful for deciding this: it's nearly tautological,
    # because a lost cycle is by definition one whose frames were not
    # acknowledged, and every non-acknowledgment generates a retry. The
    # first version of this script concluded from that difference and gave
    # a wrong verdict. The non-circular data point is RSSI, which is
    # measured the same regardless of what happens afterward.
    if dr < -2:
        print("⇒ Lost cycles arrive at the AP weaker. The loss tracks the signal")
        print("  level, and the fix is about antenna or placement.")
    else:
        print("⇒ RSSI is INDISTINGUISHABLE between the ones that arrive and the ones that don't.")
        print("  The uplink margin is equally bad in both groups, so it explains")
        print("  the REGIME —why ~1 in 4 gets lost— but not WHICH cycle gets")
        print("  lost: that's decided by a streak of failed retries exhausting")
        print("  the limit. Improving the margin should still lower the rate, and")
        print("  more than proportionally, because each dB reduces the")
        print("  per-frame failure probability and streaks are powers of that.")
        print(f"  (The {stat(ok,'retry_pct'):.0f}% RETRY rate of HEALTHY cycles is the number")
        print("  that measures the margin without circularity: a healthy link is at 1-5%.)")

# Full distribution, in case the median hides a tail.
print("\nNode RSSI per burst (median), in chronological order:")
print("  arrived: " + " ".join(f"{r['rssi_med']:.0f}" for r in ok if r["rssi_med"]))
print("  lost:    " + " ".join(f"{r['rssi_med']:.0f}" for r in lost if r["rssi_med"]))
