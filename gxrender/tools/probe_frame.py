import struct, sys

def fb_px(path, x, y):
    with open(path, 'rb') as f:
        f.seek((y*256+x)*4)
        v, = struct.unpack('<I', f.read(4))
    r, g, b, a = v & 0x3F, (v >> 8) & 0x3F, (v >> 16) & 0x3F, (v >> 24) & 0x1F
    return (r, g, b, a)

def scr_px(path, x, y, buf):
    with open(path, 'rb') as f:
        f.seek((buf*256*192 + y*256 + x)*4)
        v, = struct.unpack('<I', f.read(4))
    return (v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF)

def state_eng(path, n):
    # rebuild full state via deltas is hard; just report the raw .2d runs we know.
    pass

d = sys.argv[1]  # trace dir
for u in (14441, 14442, 14443, 14444, 14445):
    print(f'== unit {u}')
    try:
        print('  fb(0,0)   =', fb_px(f'{d}/f{u:05d}.fb', 0, 0))
        print('  fb(0,100) =', fb_px(f'{d}/f{u:05d}.fb', 0, 100))
    except FileNotFoundError:
        print('  fb missing')
    try:
        for buf in (0, 1):
            print(f'  scr buf{buf} (0,0)=', scr_px(f'{d}/f{u:05d}.scr', 0, 0, buf),
                  ' (0,100)=', scr_px(f'{d}/f{u:05d}.scr', 0, 100, buf),
                  ' (0,185)=', scr_px(f'{d}/f{u:05d}.scr', 0, 185, buf))
    except FileNotFoundError:
        print('  scr missing')
