"""Check a saved QL screen (PPM): at least N pixels of one colour.

    python3 -I tests/boot_check.py FRAME.ppm RRGGBB N
"""
import sys

path, colour, want = sys.argv[1], bytes.fromhex(sys.argv[2]), int(sys.argv[3])
d = open(path, "rb").read()
parts = d.split(b"\n", 3)
assert parts[0] == b"P6", "not a PPM"
w, h = map(int, parts[1].split())
px = parts[3]
n = sum(1 for i in range(0, w * h * 3, 3) if px[i:i + 3] == colour)
print(f"{path}: {n} pixels of #{sys.argv[2]} (want {want})")
sys.exit(0 if n >= want else 1)
