import csv
from collections import Counter
from pathlib import Path


path = Path(__file__).resolve().parents[1] / "diagnostics" / "saleae_i2s" / "digital.csv"
state = None
previous_ws = None
segments_by_mode = {name: [] for name in (
    "rise_new", "rise_old", "fall_new", "fall_old"
)}
active = {name: [] for name in segments_by_mode}

with path.open(newline="") as f:
    for row in csv.DictReader(f):
        t = float(row["Time [s]"])
        values = [int(row[f"Channel {i}"]) for i in range(5)]
        if t < 0.070:
            state = values
            previous_ws = values[1]
            continue
        if t > 0.120 or len(segments_by_mode["rise_new"]) >= 2000:
            break
        if state is None:
            state = values
            previous_ws = values[1]
            continue

        old_bclk = state[0]
        ws = values[1]
        if ws != previous_ws:
            for name in segments_by_mode:
                if active[name]:
                    segments_by_mode[name].append((previous_ws, active[name]))
                active[name] = []
            previous_ws = ws
        if old_bclk == 0 and values[0] == 1:
            active["rise_new"].append(values[2])
            active["rise_old"].append(state[2])
        if old_bclk == 1 and values[0] == 0:
            active["fall_new"].append(values[2])
            active["fall_old"].append(state[2])
        state = values

def signed16(v):
    return v - 65536 if v & 0x8000 else v

for mode, segments in segments_by_mode.items():
    decoded = {0: [], 1: []}
    lengths = Counter()
    for ws, bits in segments:
        lengths[len(bits)] += 1
        for offset in (0,):
            payload = bits[offset:offset + 16]
            if len(payload) != 16:
                continue
            value = 0
            for bit in payload:
                value = (value << 1) | bit
            decoded[ws].append(signed16(value))
    print("MODE", mode, "slot_edge_counts", dict(lengths))
    for ws in (0, 1):
        values = decoded[ws]
        print(f" WS={ws} samples={len(values)} unique={Counter(values).most_common(8)} first={values[:16]}")
