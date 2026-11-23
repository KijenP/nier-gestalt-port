import struct, sys, math
from cmf import cjf, cmf, u32

def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)

def qrot(q, v):
    x, y, z, w = q
    p = (v[0], v[1], v[2], 0.0)
    r = qmul(qmul(q, p), (-x, -y, -z, w))
    return r[:3]

def nlerp(a, b, t):
    if sum(x*y for x, y in zip(a, b)) < 0: b = tuple(-x for x in b)
    q = tuple(x + (y - x) * t for x, y in zip(a, b))
    l = math.sqrt(sum(x*x for x in q)) or 1
    return tuple(x / l for x in q)

def sample(track, f):
    keys, quats = track
    if f <= keys[0]: return quats[0]
    for i in range(1, len(keys)):
        if f <= keys[i]:
            t = (f - keys[i-1]) / max(1, keys[i] - keys[i-1])
            return nlerp(quats[i-1], quats[i], t)
    return quats[-1]

def lerp3(track, f):
    keys, vals = track
    if f <= keys[0]: return vals[0]
    for i in range(1, len(keys)):
        if f <= keys[i]:
            t = (f - keys[i-1]) / max(1, keys[i] - keys[i-1])
            return tuple(a + (b - a) * t for a, b in zip(vals[i-1], vals[i]))
    return vals[-1]

def fk(parents, loc, rots, order, trans=None):
    wr, wp = {}, {}
    for j in order:
        p = parents[j]
        t = trans[j] if trans and j in trans else loc[j][4:7]
        if p < 0:
            wr[j], wp[j] = rots[j], t
        else:
            # Row vectors (world = local * parent): the parent's rotation applies after the child's.
            wr[j] = qmul(wr[p], rots[j])
            o = qrot(wr[p], t)
            wp[j] = tuple(a + b for a, b in zip(wp[p], o))
    return wr, wp

def order_of(parents):
    done, order = set(), []
    while len(order) < len(parents):
        for j, p in enumerate(parents):
            if j not in done and (p < 0 or p in done):
                done.add(j); order.append(j)
    return order

if __name__ == '__main__':
    cpath = sys.argv[1]
    names, parents, loc = cjf(cpath)
    order = order_of(parents)
    c = open(cpath, 'rb').read()
    rest_o = u32(c, 0x38)
    rots = {j: tuple(loc[j][:4]) for j in range(len(parents))}
    wr, wp = fk(parents, loc, rots, order)
    err = 0
    for j in range(len(parents)):
        m = struct.unpack_from('>16f', c, rest_o + 64 * j)
        err = max(err, max(abs(a - b) for a, b in zip(m[12:15], wp[j])))
    print('rest FK vs CJF model-space: max error %.5f' % err)
