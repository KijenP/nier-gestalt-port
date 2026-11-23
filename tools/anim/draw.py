import sys, zlib, struct
from cmf import cjf, cmf
from pose import fk, order_of, sample, lerp3

def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def line(px, w, h, a, b, col):
    n = int(max(abs(b[0] - a[0]), abs(b[1] - a[1]))) + 1
    for i in range(n + 1):
        x = int(a[0] + (b[0] - a[0]) * i / n); y = int(a[1] + (b[1] - a[1]) * i / n)
        for dx in (0, 1):
            for dy in (0, 1):
                if 0 <= x + dx < w and 0 <= y + dy < h:
                    o = ((y + dy) * w + x + dx) * 3; px[o:o + 3] = col

# draw.py <CJF> <out.png> <CMF> [more CMF...]: 6 frames of each animation, front (x,y) and side (z,y)
cpath, out, anims = sys.argv[1], sys.argv[2], sys.argv[3:]
names, parents, loc = cjf(cpath)
order = order_of(parents)
skip = {j for j, n in enumerate(names) if n.startswith('AS_') or 'WEAPON' in n}
cell, cols = 160, 6
w, h = cell * cols * 2, cell * len(anims)
px = bytearray([24, 24, 28] * w * h)
for r, path in enumerate(anims):
    name, frames, joints, tracks, _, moves = cmf(path)
    print(name, frames, 'moves', len(moves))
    for c in range(cols):
        f = (frames - 1) * c / (cols - 1)
        rots = {j: (sample(tracks[j], f) if j in tracks else tuple(loc[j][:4])) for j in range(len(parents))}
        trans = {j: lerp3(m, f) for j, m in moves.items()}
        if 0 in trans: trans[0] = (0.0, 0.0, 0.0)  # root motion off: the figure stays in its cell
        _, wp = fk(parents, loc, rots, order, trans)
        for view in (0, 1):
            ox = (c * 2 + view) * cell + cell // 2; oy = r * cell + cell - 10
            s = cell * 0.75 / 1.8
            for j in range(len(parents)):
                p = parents[j]
                if p < 0 or j in skip or p in skip: continue
                P = lambda v: (ox + (v[0] if view == 0 else v[2]) * s, oy - v[1] * s)
                col = (230, 200, 90) if 'LEFT' in names[j] else (90, 180, 240) if 'RIGHT' in names[j] else (220, 220, 220)
                line(px, w, h, P(wp[p]), P(wp[j]), col)
png(out, w, h, px)

