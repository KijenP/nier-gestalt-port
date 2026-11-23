import struct, sys, math

def u32(d, o): return struct.unpack_from('>I', d, o)[0]

def cjf(path):
    c = open(path, 'rb').read()
    n = u32(c, 0x24)
    names_o, par_o, loc_o = u32(c, 0x2C), u32(c, 0x34), u32(c, 0x40)
    names = [c[names_o + 36 * i + 4:names_o + 36 * i + 36].split(b'\0')[0].decode() for i in range(n)]
    parents = [struct.unpack_from('>h', c, par_o + 2 * i)[0] for i in range(n)]
    loc = [struct.unpack_from('>12f', c, loc_o + 48 * i) for i in range(n)]
    return names, parents, loc

def cmf(path):
    d = open(path, 'rb').read()
    frames, joints = struct.unpack_from('>HH', d, 4)
    name = d[0xC:0x30].split(b'\0')[0].decode()
    base = u32(d, 0x94)
    tracks = {}
    for j in range(joints):
        o = u32(d, base + 4 * j)
        if not o: continue
        a = base + o
        n = struct.unpack_from('>H', d, a)[0]
        keys = struct.unpack_from('>%dH' % n, d, a + 2)
        q = a + 2 + 2 * n
        q = (q + 3) & ~3
        quats = [tuple(v / 32767.0 for v in struct.unpack_from('>4h', d, q + 8 * k)) for k in range(n)]
        tracks[j] = (keys, quats)
    # Translations: a second table of one offset per joint (relative to the table) at the offset in 0x98.
    moves = {}
    tb = u32(d, 0x98)
    if tb and tb + 4 * joints <= len(d):
        for j in range(joints):
            o = u32(d, tb + 4 * j)
            if not o: continue
            a = tb + o
            n = struct.unpack_from('>H', d, a)[0]
            keys = struct.unpack_from('>%dH' % n, d, a + 2)
            q = (a + 2 + 2 * n + 3) & ~3
            moves[j] = (keys, [struct.unpack_from('>3f', d, q + 12 * k) for k in range(n)])
    return name, frames, joints, tracks, d, moves

if __name__ == '__main__':
    names, parents, loc = cjf(sys.argv[1])
    name, frames, joints, tracks, d, moves = cmf(sys.argv[2])
    print(name, 'frames', frames, 'joints', joints, 'tracks', len(tracks))
    print('header 0x40..0x94:', d[0x40:0x98].hex())
    for j, (keys, quats) in list(tracks.items())[:20]:
        r = loc[j][:4]
        q = quats[0]
        dot = abs(sum(a * b for a, b in zip(r, q)))
        ln = math.sqrt(sum(v * v for v in q))
        print('%3d %-22s par %3d keys %2d last %3d |q| %.3f dot(rest) %.3f rest %s q0 %s' % (
            j, names[j], parents[j], len(keys), keys[-1], ln, dot,
            ' '.join('%.2f' % v for v in r), ' '.join('%.2f' % v for v in q)))

