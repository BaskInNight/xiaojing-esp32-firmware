import csv
from pathlib import Path


path = Path(__file__).resolve().parents[1] / "diagnostics" / "saleae_i2s" / "digital.csv"
rows = []
with path.open(newline="") as f:
    for row in csv.DictReader(f):
        t = float(row["Time [s]"])
        if 0.0 <= t <= 1.0:
            rows.append((t, *(int(row[f"Channel {i}"]) for i in range(5))))

if not rows:
    raise SystemExit("no triggered data")

previous = rows[0][1:]
edges = [[], [], [], [], []]
high_seen = [bool(v) for v in previous]
low_seen = [not bool(v) for v in previous]
for row in rows[1:]:
    t, values = row[0], row[1:]
    for i, (old, new) in enumerate(zip(previous, values)):
        high_seen[i] |= bool(new)
        low_seen[i] |= not bool(new)
        if old != new:
            edges[i].append(t)
    previous = values

for i in range(5):
    duration = rows[-1][0] - rows[0][0]
    edge_rate = len(edges[i]) / duration if duration else 0.0
    estimated_hz = edge_rate / 2.0
    print(
        f"D{i}: edges={len(edges[i])} high={high_seen[i]} low={low_seen[i]} "
        f"estimated_periodic_hz={estimated_hz:.3f}"
    )
