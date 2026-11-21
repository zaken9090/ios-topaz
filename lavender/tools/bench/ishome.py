"""ishome.py PNG: exit 0 if the screenshot shows the home screen (the dock: a light band at the bottom), 1 if not."""
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


def lum(y0, y1):
    s = n = 0
    for y in range(y0, y1, 4):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(0, w * 3, 30):
            s += row[x] + row[x + 1] + row[x + 2]
            n += 3
    return s / n


dock = lum(int(h * 0.915), int(h * 0.94))      # between the dock icons' tops and the bottom edge
above = lum(int(h * 0.80), int(h * 0.83))      # wallpaper above the dock
print('dock %.0f above %.0f' % (dock, above))
sys.exit(0 if dock > above + 25 else 1)
