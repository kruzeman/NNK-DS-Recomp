#!/usr/bin/env python3
"""gl_vs_soft.py -- per-pixel diff of the OpenGL engine output vs the
software reference output (both in the packed .raw layout: 256*192 u32 LE,
r|g<<8|b<<16|a<<24 with 6-bit rgb and 5-bit alpha).

Follows the gx_tracediff.py conventions (compared/differing/
max_channel_delta/VERDICT) and adds the classification the GL validation
needs: deltas are bucketed so float-vs-integer rounding deltas (GL
perspective-correct float interpolation vs the DS 8/9-bit perspective
factor, GL pixel-center fill vs the melonDS span fill) can be told apart
from real pipeline divergences.

Classes per differing pixel (max channel delta over r,g,b (0..63) and a
(0..31)):
  R1    all deltas <= 1                float/int rounding at 6-bit level
  R2    all deltas <= 3                perspective-factor quantization
  R3    all deltas <= 15               interpolation / small fill sliver
  BIG   any delta > 15                texel selection / fill-rule gaps
  + alpha_mismatch when the 5-bit alpha differs (AA coverage / blend class)

usage: gl_vs_soft.py SOFT_DIR GL_DIR [--units A-B] [--csv FILE] [--quiet]
"""
import csv
import os
import struct
import sys

W, H = 256, 192


def load_raw(path):
    raw = open(path, 'rb').read()
    if len(raw) != W * H * 4:
        raise ValueError(f"{path}: bad size {len(raw)}")
    return raw


def diff_unit(a, b):
    n = W * H
    exact = 0
    diff_px = 0
    sum_delta = 0
    max_delta = 0
    color_mismatch = 0
    alpha_mismatch = 0
    cls = {'R1': 0, 'R2': 0, 'R3': 0, 'BIG': 0}
    for i in range(n):
        va = struct.unpack_from('<I', a, i * 4)[0]
        vb = struct.unpack_from('<I', b, i * 4)[0]
        if va == vb:
            exact += 1
            continue
        diff_px += 1
        pmax = 0
        for shift in (0, 8, 16, 24):
            d = abs(((va >> shift) & 0x3F) - ((vb >> shift) & 0x3F))
            sum_delta += d
            if d > pmax:
                pmax = d
            if d > max_delta:
                max_delta = d
        if (va & 0xFFFFFF) != (vb & 0xFFFFFF):
            color_mismatch += 1
        if (va >> 24) != (vb >> 24):
            alpha_mismatch += 1
        if pmax <= 1:
            cls['R1'] += 1
        elif pmax <= 3:
            cls['R2'] += 1
        elif pmax <= 15:
            cls['R3'] += 1
        else:
            cls['BIG'] += 1
    return {
        'unit_px': n, 'exact': exact, 'diff_px': diff_px,
        'sum_delta': sum_delta, 'max_delta': max_delta,
        'color_mismatch': color_mismatch, 'alpha_mismatch': alpha_mismatch,
        'R1': cls['R1'], 'R2': cls['R2'], 'R3': cls['R3'], 'BIG': cls['BIG'],
    }


def main():
    args = sys.argv[1:]
    soft_dir = gl_dir = None
    units_spec = None
    csv_path = None
    quiet = False
    i = 0
    while i < len(args):
        if args[i] == '--units':
            units_spec = args[i + 1]; i += 2
        elif args[i] == '--csv':
            csv_path = args[i + 1]; i += 2
        elif args[i] == '--quiet':
            quiet = True; i += 1
        elif soft_dir is None:
            soft_dir = args[i]; i += 1
        elif gl_dir is None:
            gl_dir = args[i]; i += 1
        else:
            print(f'unknown arg {args[i]}', file=sys.stderr)
            return 2
    if not soft_dir or not gl_dir:
        print(__doc__, file=sys.stderr)
        return 2

    lo = hi = None
    if units_spec:
        if '-' in units_spec:
            lo, hi = (int(x) for x in units_spec.split('-'))
        else:
            lo = hi = int(units_spec)

    rows = []
    units = []
    for fn in sorted(os.listdir(soft_dir)):
        if not (fn.startswith('f') and fn.endswith('.raw')):
            continue
        unit = int(fn[1:6])
        if lo is not None and hi is not None and not (lo <= unit <= hi):
            continue
        if not os.path.exists(os.path.join(gl_dir, fn)):
            continue
        units.append(unit)

    tot = dict(unit_px=0, exact=0, diff_px=0, sum_delta=0, max_delta=0,
               color_mismatch=0, alpha_mismatch=0, R1=0, R2=0, R3=0, BIG=0)
    print(f'A={soft_dir} (software reference)')
    print(f'B={gl_dir} (OpenGL engine)')
    print(f'units={len(units)}')
    if not quiet:
        print('unit   exact%     diff_px  maxd  mean_d  alpha_mm  R1    R2    R3    BIG')
    for unit in units:
        fn = f'f{unit:05d}.raw'
        r = diff_unit(load_raw(os.path.join(soft_dir, fn)),
                      load_raw(os.path.join(gl_dir, fn)))
        rows.append((unit, r))
        for k in tot:
            if k == 'max_delta':
                tot[k] = max(tot[k], r[k])
            elif k == 'unit_px':
                tot[k] += r[k]
            else:
                tot[k] += r[k]
        if not quiet:
            mean_d = r['sum_delta'] / (4 * r['unit_px'])
            print(f'{unit:5d}  {100.0*r["exact"]/r["unit_px"]:8.4f}%  '
                  f'{r["diff_px"]:8d}  {r["max_delta"]:4d}  {mean_d:5.3f}  '
                  f'{r["alpha_mismatch"]:8d}  {r["R1"]:<5d} {r["R2"]:<5d} '
                  f'{r["R3"]:<5d} {r["BIG"]}')

    n = tot['unit_px']
    print(f'TOTAL  {100.0*tot["exact"]/n:8.4f}%  {tot["diff_px"]:8d}  '
          f'{tot["max_delta"]:4d}  {tot["sum_delta"]/(4*n):5.3f}  '
          f'{tot["alpha_mismatch"]:8d}  {tot["R1"]:<5d} {tot["R2"]:<5d} '
          f'{tot["R3"]:<5d} {tot["BIG"]}')
    print(f'classes: R1(<=1)={tot["R1"]} R2(<=3)={tot["R2"]} '
          f'R3(<=15)={tot["R3"]} BIG(>15)={tot["BIG"]} '
          f'color_mm={tot["color_mismatch"]} alpha_mm={tot["alpha_mismatch"]}')
    if tot['diff_px'] == 0:
        verdict = 'IDENTICAL'
    else:
        verdict = f'DIFFERENT ({100.0*tot["diff_px"]/n:.2f}% of pixels differ)'
    print(f'VERDICT: {verdict}')

    if csv_path:
        with open(csv_path, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['unit', 'exact', 'diff_px', 'max_delta', 'sum_delta',
                        'color_mismatch', 'alpha_mismatch', 'R1', 'R2', 'R3',
                        'BIG'])
            for unit, r in rows:
                w.writerow([unit, r['exact'], r['diff_px'], r['max_delta'],
                            r['sum_delta'], r['color_mismatch'],
                            r['alpha_mismatch'], r['R1'], r['R2'], r['R3'],
                            r['BIG']])
    return 0


if __name__ == '__main__':
    sys.exit(main())
