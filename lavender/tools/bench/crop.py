import sys, zlib, struct
src, out, x0, y0, w0, h0, k = sys.argv[1], sys.argv[2], *map(int, sys.argv[3:8])
d = open(src, 'rb').read()
w, h = struct.unpack('>II', d[16:24]); ct = d[25]
idat = b''; i = 8
while i < len(d):
    n = struct.unpack('>I', d[i:i+4])[0]
    if d[i+4:i+8] == b'IDAT': idat += d[i+8:i+8+n]
    i += 12 + n
raw = zlib.decompress(idat); bpp = 3 if ct == 2 else 4; stride = w*bpp + 1
# unfilter
img = bytearray(); prev = bytearray(w*bpp)
for y in range(h):
    f = raw[y*stride]; line = bytearray(raw[y*stride+1:(y+1)*stride])
    for x in range(len(line)):
        a = line[x-bpp] if x >= bpp else 0; b = prev[x]; c = prev[x-bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b)//2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    img += line; prev = line
rows = []
for y in range(y0, y0+h0):
    r = bytearray()
    for x in range(x0, x0+w0):
        px = img[(y*w+x)*bpp:(y*w+x)*bpp+3]; r += px*k
    for _ in range(k): rows.append(b'\0' + bytes(r))
W, H = w0*k, h0*k
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t+b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
print(w, h, ct)
