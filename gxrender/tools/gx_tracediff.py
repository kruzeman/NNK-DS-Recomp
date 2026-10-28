#!/usr/bin/env python3
"""gx_tracediff.py -- per-pixel diff of gxrender replay output vs melonDS.

Compares melonDS trace f%05u.fb (3D layer dumps) against gx_replay output
f%05u.raw (same layout: 256*192 u32 LE, r|g<<8|b<<16|a<<24, 6-bit rgb +
5-bit alpha).

Follows the DQ8 compare_frames.py conventions (compared/differing/
max_channel_delta/VERDICT) extended with per-pixel stats and a report table.

usage: gx_tracediff.py TRACE_DIR REPLAY_DIR [--units A[-B]] [--png DIR]
                       [--csv FILE] [--quiet]
"""
import csv
import os
import struct
import sys

W, H = 256, 192


def load_fb(path):
    raw = open(path, 'rb').read()
    if len(raw) != W * H * 4:
        raise ValueError(f"{path}: bad size {len(raw)}")
    return raw


def diff_unit(a, b):
    """a = melonDS fb bytes, b = replay raw bytes. Returns dict."""
    n = W * H
    exact = 0
    diff_px = 0
    sum_delta = 0
    max_delta = 0
    color_mismatch = 0   # rgb differs (alpha may or may not)
    alpha_mismatch = 0   # alpha differs
    cov_a = 0
    cov_b = 0
    for i in range(n):
        va = struct.unpack_from("<I", a, i * 4)[0]
        vb = struct.unpack_from("<I", b, i * 4)[0]
        if va & 0xFFFFFF:
            cov_a += 1
        if vb & 0xFFFFFF:
            cov_b += 1
        if va == vb:
            exact += 1
            continue
        diff_px += 1
        # per-channel deltas: r,g,b in bits 0-5/8-13/16-21, a in bits 24-28
        for shift in (0, 8, 16, 24):
            d = abs(((va >> shift) & 0x3F) - ((vb >> shift) & 0x3F))
            sum_delta += d
            if d > max_delta:
                max_delta = d
        if (va & 0xFFFFFF) != (vb & 0xFFFFFF):
            color_mismatch += 1
        if (va >> 24) != (vb >> 24):
            alpha_mismatch += 1
    return {
        "pixels": n,
        "exact": exact,
        "diff_px": diff_px,
        "exact_pct": 100.0 * exact / n,
        "mean_delta": sum_delta / (diff_px * 4) if diff_px else 0.0,
        "max_delta": max_delta,
        "color_mismatch": color_mismatch,
        "alpha_mismatch": alpha_mismatch,
        "cov_melonds": 100.0 * cov_a / n,
        "cov_replay": 100.0 * cov_b / n,
    }


def write_pngs(mel, rep, unit, pngdir):
    from PIL import Image
    img_a = Image.new("RGB", (W, H))
    img_b = Image.new("RGB", (W, H))
    img_d = Image.new("RGB", (W, H))
    pa, pb, pd = img_a.load(), img_b.load(), img_d.load()
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 4
            va = struct.unpack_from("<I", mel, i)[0]
            vb = struct.unpack_from("<I", rep, i)[0]
            pa[x, y] = ((va & 0x3F) << 2, ((va >> 8) & 0x3F) << 2, ((va >> 16) & 0x3F) << 2)
            pb[x, y] = ((vb & 0x3F) << 2, ((vb >> 8) & 0x3F) << 2, ((vb >> 16) & 0x3F) << 2)
            if va == vb:
                pd[x, y] = (0, 0, 0)
            else:
                d = 0
                for shift in (0, 8, 16):
                    d = max(d, abs(((va >> shift) & 0x3F) - ((vb >> shift) & 0x3F)))
                pd[x, y] = (min(255, d * 8), 0, 0)
    img_a.save(os.path.join(pngdir, f"f{unit:05d}_melonds.png"))
    img_b.save(os.path.join(pngdir, f"f{unit:05d}_replay.png"))
    img_d.save(os.path.join(pngdir, f"f{unit:05d}_diff.png"))


def main():
    args = sys.argv[1:]
    pngdir = csvfile = None
    quiet = False
    units_range = None
    positional = []
    i = 0
    while i < len(args):
        if args[i] == "--png":
            pngdir = args[i + 1]
            i += 2
        elif args[i] == "--csv":
            csvfile = args[i + 1]
            i += 2
        elif args[i] == "--units":
            a = args[i + 1]
            if "-" in a:
                lo, hi = a.split("-")
                units_range = (int(lo), int(hi))
            else:
                units_range = (int(a), int(a))
            i += 2
        elif args[i] == "--quiet":
            quiet = True
            i += 1
        else:
            positional.append(args[i])
            i += 1
    if len(positional) != 2:
        print(__doc__)
        return 2
    tdir, rdir = positional
    if pngdir:
        os.makedirs(pngdir, exist_ok=True)

    rows = []
    for name in sorted(os.listdir(tdir)):
        if not name.endswith(".fb"):
            continue
        unit = int(name[1:6])
        if units_range and not (units_range[0] <= unit <= units_range[1]):
            continue
        rpath = os.path.join(rdir, f"f{unit:05d}.raw")
        if not os.path.exists(rpath):
            continue
        mel = load_fb(os.path.join(tdir, name))
        rep = load_fb(rpath)
        st = diff_unit(mel, rep)
        st["unit"] = unit
        rows.append(st)
        if pngdir:
            write_pngs(mel, rep, unit, pngdir)
        if not quiet:
            print(f"unit {unit}: exact={st['exact_pct']:.2f}% "
                  f"diff_px={st['diff_px']} max_delta={st['max_delta']} "
                  f"cov mel={st['cov_melonds']:.1f}% rep={st['cov_replay']:.1f}%")

    if not rows:
        print("no comparable units")
        return 1

    npx = rows[0]["pixels"]
    tot_exact = sum(r["exact"] for r in rows)
    tot_diff = sum(r["diff_px"] for r in rows)
    tot = len(rows) * npx
    max_delta = max(r["max_delta"] for r in rows)
    print()
    print(f"A={tdir} units={len(rows)}")
    print(f"B={rdir}")
    print(f"compared={tot} differing_pixels={tot_diff} "
          f"({100.0 * tot_diff / tot:.3f}%) max_channel_delta={max_delta}")
    print(f"exact_match_avg={100.0 * tot_exact / tot:.2f}%")
    if csvfile:
        with open(csvfile, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    identical = all(r["diff_px"] == 0 for r in rows)
    print("VERDICT:", "IDENTICAL" if identical else
          f"DIFFERENT ({100.0 * tot_diff / tot:.2f}% of pixels differ)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
