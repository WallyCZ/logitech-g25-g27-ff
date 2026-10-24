"""Summarize a hidraw input capture: per byte value range and activity windows, and per bit toggle times.

usage: python analyze-capture.py capture.txt
"""
import sys

rows = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    parts = line.split()
    if len(parts) < 3 or parts[1] != "in:":
        continue
    try:
        data = [int(x, 16) for x in parts[2:]]
    except ValueError:
        continue
    if rows and len(data) != len(rows[0][1]):
        continue  # truncated last line
    rows.append((int(parts[0]), data))
t0 = rows[0][0]
n = len(rows[0][1])
print(f"{len(rows)} reports over {(rows[-1][0] - t0) / 1000:.1f} s, {n} bytes; first: {' '.join(f'{b:02X}' for b in rows[0][1])}")


def windows(times, gap=1500):
    """Group change times (ms since start) into activity windows separated by > gap ms."""
    out = []
    for t in times:
        if out and t - out[-1][1] <= gap:
            out[-1][1] = t
        else:
            out.append([t, t])
    return out


def fmt(ws):
    return ", ".join(f"{a / 1000:.1f}-{b / 1000:.1f}" for a, b in ws)


print("\n== bytes ==")
for i in range(n):
    vals = [r[1][i] for r in rows]
    changes = [rows[k][0] - t0 for k in range(1, len(rows)) if rows[k][1][i] != rows[k - 1][1][i]]
    if not changes:
        print(f"byte {i:2}: constant {vals[0]:02X}")
        continue
    print(f"byte {i:2}: {min(vals):02X}..{max(vals):02X}, {len(changes)} changes, active s: {fmt(windows(changes))}")

print("\n== bits (bytes with few distinct values) ==")
for i in range(n):
    vals = [r[1][i] for r in rows]
    if len(set(vals)) == 1 or len(set(vals)) > 40:
        continue
    for bit in range(8):
        m = 1 << bit
        tog = [rows[k][0] - t0 for k in range(1, len(rows)) if (rows[k][1][i] ^ rows[k - 1][1][i]) & m]
        if tog:
            print(f"byte {i:2} bit {bit}: {len(tog):4} toggles at s: {fmt(windows(tog, 300))}")
