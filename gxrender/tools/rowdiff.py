import struct, sys

def load(path):
    with open(path, 'rb') as f:
        hdr = f.read(15)  # "P6\n256 192\n255\n"
        return f.read(256*192*3)

base = sys.argv[1]
for buf in ('0', '1'):
    try:
        rep = load(f'{base}/f14444_s{buf}.ppm')
        gold = load(f'{base}/f14444_g{buf}.ppm')
    except FileNotFoundError:
        continue
    print(f'--- buffer {buf} ---')
    for y in range(192):
        d = 0
        for x in range(256):
            i = (y*256+x)*3
            if rep[i:i+3] != gold[i:i+3]:
                d += 1
        if d:
            # sample first differing pixel and gold/rep values
            for x in range(256):
                i = (y*256+x)*3
                if rep[i:i+3] != gold[i:i+3]:
                    print(f'row {y:3d}: {d:3d} diff px, x={x} rep={list(rep[i:i+3])} gold={list(gold[i:i+3])}')
                    break
    # per-row boundary summary
    rows = [y for y in range(192) if any(rep[(y*256+x)*3:(y*256+x)*3+3] != gold[(y*256+x)*3:(y*256+x)*3+3] for x in range(256))]
    print(f'differing rows: {rows[:5]}...{rows[-5:] if rows else []} count={len(rows)}')
