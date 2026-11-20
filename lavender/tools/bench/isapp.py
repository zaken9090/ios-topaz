"""isapp.py PNG: exit 0 if the screenshot looks like Settings (light grey background), 1 if not (home's blue)."""
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('>II', d[16:24])
idat = b''
i = 8
while i < len(d):
    n = struct.unpack('>I', d[i:i + 4])[0]
    if d[i + 4:i + 8] == b'IDAT':
        idat += d[i + 8:i + 8 + n]
    i += 12 + n
raw = zlib.decompress(idat)
stride = w * 3 + 1
light = 0
for y in (h // 3, h // 2, 2 * h // 3):
    row = raw[y * stride + 1:(y + 1) * stride]
    for x in range(0, w * 3, 30):
        r, g, b = row[x], row[x + 1], row[x + 2]
        if r > 200 and g > 200 and b > 200:
            light += 1
total = 3 * len(range(0, w * 3, 30))
print('light %d/%d' % (light, total))
sys.exit(0 if light > total // 3 else 1)
