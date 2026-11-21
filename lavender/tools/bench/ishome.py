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


def blue(y0, y1):
    """mean blue minus red: the wallpaper is blue, the dock (light or, with Reduce Transparency, dark) grey"""
    s = n = 0
    for y in range(y0, y1, 4):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(0, w * 3, 30):
            s += row[x + 2] - row[x]
            n += 1
    return s / n


dock = blue(int(h * 0.915), int(h * 0.94)), blue(int(h * 0.975), int(h * 0.99))   # dock above and below its icons
above = blue(int(h * 0.80), int(h * 0.83))      # wallpaper above the dock
print('blue-red: dock %.0f %.0f, above %.0f' % (dock[0], dock[1], above))
ldock = lum(int(h * 0.915), int(h * 0.94))
labove = lum(int(h * 0.80), int(h * 0.83))
print('light: dock %.0f above %.0f' % (ldock, labove))
# a light dock over the wallpaper, or a grey (dark) one over a blue wallpaper
sys.exit(0 if ldock > labove + 25 or (above > 40 and max(dock) < above / 2) else 1)
