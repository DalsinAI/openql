"""Check a saved QL screen (PPM): at least N pixels of one colour.

    python3 -I tests/boot_check.py FRAME.ppm RRGGBB N [X0,Y0,X1,Y1]

With a box, only the pixels inside it count (X1 and Y1 not included).
"""
import sys

path, colour, want = sys.argv[1], bytes.fromhex(sys.argv[2]), int(sys.argv[3])
d = open(path, "rb").read()
parts = d.split(b"\n", 3)
assert parts[0] == b"P6", "not a PPM"
w, h = map(int, parts[1].split())
x0, y0, x1, y1 = map(int, sys.argv[4].split(",")) if len(sys.argv) > 4 else (0, 0, w, h)
px = parts[3]
n = sum(1 for y in range(max(y0, 0), min(y1, h)) for x in range(max(x0, 0), min(x1, w))
        if px[(y * w + x) * 3:(y * w + x) * 3 + 3] == colour)
box = f" in {x0},{y0},{x1},{y1}" if len(sys.argv) > 4 else ""
print(f"{path}: {n} pixels of #{sys.argv[2]}{box} (want {want})")
sys.exit(0 if n >= want else 1)
