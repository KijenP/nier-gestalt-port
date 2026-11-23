"""Prints the PowerPC of recompiled game functions (the // comments in generated/default/*.cpp).

  python tools/asm.py 8238D248 [8238D2D0 ...] [--max N]

An index (generated/default/asm_index.json: function -> file, line) is made on the first run.
"""
import json, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'generated', 'default')
INDEX = os.path.join(ROOT, 'asm_index.json')


def index():
    if os.path.exists(INDEX):
        return json.load(open(INDEX))
    idx = {}
    for name in os.listdir(ROOT):
        if not (name.startswith('nier_recomp.') and name.endswith('.cpp')):
            continue
        with open(os.path.join(ROOT, name), encoding='utf-8', errors='replace') as f:
            for n, line in enumerate(f):
                m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', line)
                if m:
                    idx[m.group(1)] = [name, n]
    json.dump(idx, open(INDEX, 'w'))
    return idx


def dump(idx, addr, limit):
    addr = addr.upper().replace('0X', '').replace('SUB_', '')
    if addr not in idx:
        # inside a function: the one starting just before
        starts = sorted(int(a, 16) for a in idx)
        a = int(addr, 16)
        before = [s for s in starts if s <= a]
        if not before:
            print('== %s: no function' % addr)
            return
        addr = '%08X' % before[-1]
        print('(inside sub_%s)' % addr)
    name, line = idx[addr]
    out = []
    with open(os.path.join(ROOT, name), encoding='utf-8', errors='replace') as f:
        for n, l in enumerate(f):
            if n < line:
                continue
            if n > line and l.startswith('DEFINE_REX_FUNC'):
                break
            s = l.strip()
            if s.startswith('// '):
                out.append(s[3:])
            elif re.match(r'loc_[0-9A-F]{8}:', s):
                out.append(s)
            if len(out) >= limit:
                break
    print('== sub_%s (%s:%d), %d lines' % (addr, name, line + 1, len(out)))
    print('\n'.join(out))


if __name__ == '__main__':
    args = sys.argv[1:]
    limit = 100000
    if '--max' in args:
        i = args.index('--max')
        limit = int(args[i + 1])
        del args[i:i + 2]
    idx = index()
    for a in args:
        dump(idx, a, limit)
