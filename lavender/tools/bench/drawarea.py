"""drawarea.py LOG FRAME: per draw of a gltrace frame: program, framebuffer (size), textures, the area its corners cover
(clipped to the framebuffer), and totals per framebuffer."""
import re, sys
from collections import defaultdict

data = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
frame = sys.argv[2]
lines = re.findall(r'gl: ([^|\n]*)', data)
start = None
for i, l in enumerate(lines):
    if l.startswith('---- frame %s ' % frame):
        start = i
        break
if start is None:
    sys.exit('frame not found')
draws = []
cur = None
fbsize = {}
for l in lines[start + 1:]:
    if l.startswith('---- frame') or l.startswith('frame '):
        break
    if l.startswith('glDraw'):
        cur = {'call': l, 'xs': [], 'ys': [], 'prog': '?', 'fb': '?', 'tex': ''}
        draws.append(cur)
        continue
    if cur is None:
        continue
    m = re.match(r'\s*program (\d+), framebuffer (\d+), textures(.*)', l)
    if m:
        cur['prog'], cur['fb'], cur['tex'] = m.group(1), m.group(2), m.group(3).strip()
        continue
    m = re.match(r'\s*corner \d+: \[0\] (\S+) (\S+)', l)
    if m:
        try:
            cur['xs'].append(float(m.group(1)))
            cur['ys'].append(float(m.group(2)))
        except ValueError:
            pass
        continue
    m = re.match(r'\s*framebuffer (\d+) \((\d+) x (\d+)\)', l)
    if m:
        fbsize[m.group(1)] = (int(m.group(2)), int(m.group(3)))
tot = defaultdict(float)
cnt = defaultdict(int)
for d in draws:
    w, h = fbsize.get(d['fb'], (1125, 2436))
    if d['xs']:
        x0, x1 = max(0, min(d['xs'])), min(w, max(d['xs']))
        y0, y1 = max(0, min(d['ys'])), min(h, max(d['ys']))
        a = max(0, x1 - x0) * max(0, y1 - y0)
    else:
        a = 0
    tot[d['fb']] += a
    cnt[d['fb']] += 1
    print('%-6s fb %-3s %4dx%-4d %7.2f Mpx  %s' % (d['prog'], d['fb'], w, h, a / 1e6, d['tex'][:90]))
print('draws %d' % len(draws))
for fb in tot:
    print('framebuffer %s %s: %d draws, %.1f Mpx' % (fb, fbsize.get(fb), cnt[fb], tot[fb] / 1e6))
print('all: %.1f Mpx' % (sum(tot.values()) / 1e6))
