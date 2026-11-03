#!/usr/bin/env python3
"""Phase-5BM: read a run's captured frames instead of guessing at them.

The post-skip question of Phase-5BM ("are the screens still black?") is a
question about *frames that are already on disk*: `NNK_GX_SHOT=<base>
NNK_GX_SHOT_EVERY=<n>` writes `<base>.<frame>.top.ppm` / `<base>.<frame>.bot.ppm`
for every presented frame, and a wrong conclusion costs a whole phase.  This
tool answers the three questions the 5BM card asks, from those files:

  first-nonblack <base> <top|bot> [from-frame] [threshold]
      the first captured frame `<base>.<frame>.<screen>.ppm` at or after
      `from-frame` whose mean luma is above `threshold` (default 2.0), printed
      as "<frame> mean=<m>".  Exit 1 when every captured frame is still black.
      `<base>` is the shot base given to `NNK_GX_SHOT`, so one run is never
      mixed with another (work/p5bm/c, work/pb5m/b, ...).

  darkband <file.ppm> [x0 x1 y0 y1] [threshold]
      the number of dark pixels inside a rectangle (default x70..200,
      y88..104, threshold 128) -- the Phase-5BI/5BJ prompt-text measurement
      (219 dark px at frame 6400 on a cold boot).

  corr <file.yuv> <file.ppm> [ref-frame ...]
      Pearson correlation of the PPM's luma against yuv420p reference frames
      (default: print the best match over the whole stream).  Used for the
      intro gate "top screen vs work/p5t/op_u.yuv" (f4200 -> f27 0.9978,
      f4300 -> f40 0.9983 on the delivered build).

Frame numbers are **display frames**, i.e. the same counter the .nnk input
script uses and the same one printed in the `NNK_INPUT_TRACE` lines.
"""
import glob
import os
import sys

FW, FH = 256, 192
FSZ = FW * FH * 3 // 2


def read_ppm(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit('not a binary PPM: %s' % path)
    i, vals = 2, []
    while len(vals) < 3:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] not in (b'\n', b''):
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        vals.append(int(d[i:j]))
        i = j
    i += 1
    return vals[0], vals[1], d[i:]


def luma_rows(path):
    w, h, px = read_ppm(path)
    return w, h, [(px[k * 3] * 77 + px[k * 3 + 1] * 150 + px[k * 3 + 2] * 29) >> 8
                  for k in range(w * h)]


def mean_luma(path):
    w, h, l = luma_rows(path)
    return sum(l) / len(l)


def frame_of(path):
    # <base>.<frame>.<screen>.ppm  (and .ppm.png for converted copies)
    name = os.path.basename(path)
    parts = name.split('.')
    for p in parts:
        if p.isdigit() and len(p) >= 5:
            return int(p)
    raise SystemExit('cannot read a frame number from %s' % name)


def cmd_first_nonblack(base, screen, from_frame=0, threshold=2.0):
    files = sorted(glob.glob('%s.*.%s.ppm' % (base, screen)))
    files = [f for f in files if frame_of(f) >= from_frame]
    if not files:
        raise SystemExit('no %s frames at or after %d for base %s' % (screen, from_frame, base))
    for f in files:
        m = mean_luma(f)
        if m > threshold:
            print('%d mean=%.1f %s' % (frame_of(f), m, os.path.basename(f)))
            return 0
    print('still black: %d frames from %d' % (len(files), from_frame))
    return 1


def cmd_darkband(path, x0=70, x1=200, y0=88, y1=104, thr=128):
    w, h, l = luma_rows(path)
    n = 0
    for y in range(y0, min(y1, h - 1) + 1):
        for x in range(x0, min(x1, w - 1) + 1):
            if l[y * w + x] < thr:
                n += 1
    print('%s dark_band=%d' % (os.path.basename(path), n))
    return 0


def corr(a, b):
    n = len(a)
    ma = sum(a) / n
    mb = sum(b) / n
    num = da = db = 0.0
    for x, y in zip(a, b):
        x -= ma
        y -= mb
        num += x * y
        da += x * x
        db += y * y
    return num / (da ** 0.5 * db ** 0.5) if da and db else 0.0


def cmd_corr(yuv_path, ppm_path, refs):
    raw = open(yuv_path, 'rb').read()
    nf = len(raw) // FSZ
    w, h, l = luma_rows(ppm_path)
    if not refs:
        best = max(((corr(l, raw[n * FSZ:n * FSZ + FW * FH]), n) for n in range(nf)))
        refs = [best[1]]
        print('best of %d frames: f%d' % (nf, best[1]))
    for n in refs:
        c = corr(l, raw[n * FSZ:n * FSZ + FW * FH])
        print('%s vs yuv f%d corr=%.4f' % (os.path.basename(ppm_path), n, c))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'first-nonblack':
        return cmd_first_nonblack(argv[2], argv[3],
                                  int(argv[4]) if len(argv) > 4 else 0,
                                  float(argv[5]) if len(argv) > 5 else 2.0)
    if cmd == 'darkband':
        rest = [int(a) for a in argv[3:7]]
        return cmd_darkband(argv[2], *rest) if rest else cmd_darkband(argv[2])
    if cmd == 'corr':
        return cmd_corr(argv[2], argv[3], [int(a) for a in argv[4:]])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
