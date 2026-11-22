import re, sys, zlib, struct
d = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
k = 6
imgs = {}
for m in re.finditer(r'gldump (\d+) (\d+) row (\d+) ([0-9a-f]+)', d):
    hx = m.group(4)
    hx = hx[:len(hx) // 8 * 8]
    if len(hx) >= 8:
        imgs.setdefault(int(m.group(2)), {})[int(m.group(3))] = bytes.fromhex(hx)
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
for n, rows in sorted(imgs.items()):
    h = max(rows) + 1; w = min(len(r) for r in rows.values()) // 4
    out = []
    for y in range(h - 1, -1, -1):          # GL rows are bottom-up
        r = rows.get(y, bytes(w * 4)); line = bytearray()
        for x in range(w):
            p = r[4*x:4*x+4]; a = p[3]
            # over a checkerboard so that transparency shows
            bg = 200 if ((x // 4) + (y // 4)) % 2 else 120
            line += bytes(min(255, p[c] + bg * (255 - a) // 255) for c in range(3)) * k
        for _ in range(k): out.append(b'\0' + bytes(line))
    W, H = w * k, h * k
    open('dump/gldump-%d.png' % n, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(out))) + chunk(b'IEND', b''))
    print(n, w, h)
