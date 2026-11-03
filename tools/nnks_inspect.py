#!/usr/bin/env python3
"""Inspect an NNKS savestate and say whether THIS build will load it.

    tools/nnks_inspect.py <state> [layout-file]

The file is self-describing since Phase-5BH: the header carries the identity
block + a `flags` word, the region table carries {id, version, length, crc,
nfields} per region, and every field-table region carries its own directory of
{name, off, len}.  A v1 file (region table of {name, offset, length, crc}, no
directory) is still read.

The classification compares the file against a LAYOUT FILE — the text dump of
the running build's region/field layout, written by the runtime:

    NNK_SAVESTATE_LAYOUT=build/nnk_layout.txt ./nnk_runner ROM   # at boot
    ... plus <state>.layout next to every F5/SAVE_AT state

Without one the stored table is still printed and the verdict says so.

Verdicts
    EXACT         every region matches this build's id/version/length: a
                  byte-for-byte restore (what NNK_SAVESTATE_STRICT=1 requires)
    PARTIAL       the file loads with a best-effort restore: the regions that
                  still match are applied, the rest are reported field by field
                  (an older build's state — the normal case)
    INCOMPATIBLE  magic, container version or a CRC is wrong: the file is
                  refused outright, the run is NOT from a loaded state
"""
import os
import struct
import sys
import zlib

HDR = struct.Struct('<4sIIIIIQIIQQQII')    # magic ver hsize nreg build flags rom rcrc acrc step frame psize pcrc hcrc
REG1 = struct.Struct('<16sQQII')           # v1: name off len crc rsv
REG2 = struct.Struct('<IIQQII16s')         # v2: id ver off len crc nfields name
FLD = struct.Struct('<24sII')              # name off len

FLAG_FIELDDIR = 1


def read_layout(path):
    """Parse a NNKLAYOUT text dump -> (build, {id: dict}, [region order])."""
    build = None
    regs = []
    cur = None
    for line in open(path):
        f = line.split()
        if not f:
            continue
        if f[0] == 'build' and len(f) > 1:
            build = int(f[1], 16)
        elif f[0] == 'region' and len(f) == 6:
            cur = {'id': int(f[1]), 'version': int(f[2], 16),
                   'len': int(f[3]), 'nfields': int(f[4]), 'name': f[5],
                   'fields': []}
            regs.append(cur)
        elif f[0] == 'field' and len(f) == 4 and cur is not None:
            cur['fields'].append((f[1], int(f[2]), int(f[3])))
    return build, regs


def main(argv):
    if len(argv) < 2:
        sys.exit('usage: nnks_inspect.py <state> [layout-file]')
    path = argv[1]
    b = open(path, 'rb').read()
    if len(b) < HDR.size:
        sys.exit('INCOMPATIBLE: %d B is shorter than the %d B header' % (len(b), HDR.size))
    (magic, ver, hsize, nreg, build, flags, rom, rcrc, acrc, step, frame,
     psize, pcrc, hcrc) = HDR.unpack_from(b, 0)
    print('file %s (%d B)' % (path, len(b)))
    print('magic=%r ver=%d flags=0x%X hsize=%d regions=%d build=0x%08X' %
          (magic, ver, flags, hsize, nreg, build))
    print('rom=%d hdr_crc=0x%08X arm9_crc=0x%08X step=%d frame=%d' %
          (rom, rcrc, acrc, step, frame))
    good = True
    if magic != b'NNKS':
        print('INCOMPATIBLE: bad magic — this is not an NNKS savestate')
        return 2
    if ver not in (1, 2):
        print('INCOMPATIBLE: container version %d (this tool reads 1 and 2)' % ver)
        return 2
    hcalc = zlib.crc32(b[:HDR.size - 4]) & 0xFFFFFFFF
    payload = b[hsize:hsize + psize]
    pcalc = zlib.crc32(payload) & 0xFFFFFFFF
    print('header crc 0x%08X (recomputed 0x%08X) %s' %
          (hcrc, hcalc, 'OK' if hcalc == hcrc else 'MISMATCH'))
    print('payload %d B crc=0x%08X (recomputed 0x%08X) %s' %
          (psize, pcrc, pcalc, 'OK' if pcalc == pcrc and len(payload) == psize
           else 'MISMATCH'))
    if hcalc != hcrc or pcalc != pcrc or len(payload) != psize:
        good = False

    # ---- region table ----
    regs = []
    off = HDR.size
    if ver == 1:
        for i in range(nreg):
            name, roff, rlen, rcrc_, _rsv = REG1.unpack_from(b, off + i * REG1.size)
            regs.append({'id': 0, 'version': 0, 'off': roff, 'len': rlen,
                         'crc': rcrc_, 'nfields': 0,
                         'name': name.decode('ascii', 'replace').rstrip('\0'),
                         'dir': None})
        dirdata_off = HDR.size + nreg * REG1.size
    else:
        total_dir = 0
        for i in range(nreg):
            rid, rver, roff, rlen, rcrc_, nf, name = REG2.unpack_from(b, off + i * REG2.size)
            regs.append({'id': rid, 'version': rver, 'off': roff, 'len': rlen,
                         'crc': rcrc_, 'nfields': nf,
                         'name': name.decode('ascii', 'replace').rstrip('\0'),
                         'dir': None})
            if flags & FLAG_FIELDDIR:
                total_dir += nf
        dirdata_off = HDR.size + nreg * REG2.size
        if total_dir:
            for r in regs:
                r['dir'] = []
                for k in range(r['nfields']):
                    nm, foff, flen = FLD.unpack_from(b, dirdata_off + k * FLD.size)
                    r['dir'].append((nm.decode('ascii', 'replace').rstrip('\0'), foff, flen))
                dirdata_off += r['nfields'] * FLD.size
    print('\nregion table (%s, %d entries):' % ('v1, matched by NAME' if ver == 1
                                                else 'v2, matched by ID', nreg))
    print('  id  %-14s version      length        crc         nfields  status' % 'name')
    for r in regs:
        data = b[hsize + r['off']:hsize + r['off'] + r['len']]
        rc = zlib.crc32(data) & 0xFFFFFFFF
        print('  %-3d %-14s 0x%08X %-12d 0x%08X%s %-8d %s' %
              (r['id'], r['name'], r['version'], r['len'], r['crc'],
               '' if rc == r['crc'] else '!', r['nfields'],
               'not in this build' if r['id'] > 11 else ''))
        if rc != r['crc']:
            good = False

    # ---- the running build's layout ----
    lay = argv[2] if len(argv) > 2 else os.environ.get('NNK_SAVESTATE_LAYOUT')
    if not lay:
        cand = path + '.layout'
        if os.path.isfile(cand):
            lay = cand
        else:
            cand = os.path.join(os.path.dirname(os.path.abspath(path)), 'nnk_layout.txt')
            if os.path.isfile(cand):
                lay = cand
    if not lay or not os.path.isfile(lay):
        print('\nverdict: header/CRC checked only%s'
              % ('' if good else ' — the file is CORRUPT/INCOMPATIBLE'))
        print('  no build layout available: pass one as argv[2] or set'
              ' NNK_SAVESTATE_LAYOUT (the runtime writes it:'
              ' NNK_SAVESTATE_LAYOUT=nnk_layout.txt ./nnk_runner ROM)')
        return 0 if good else 2

    lbuild, lregs = read_layout(lay)
    print('\nbuild layout %s: build=0x%08X regions=%d' %
          (lay, lbuild if lbuild is not None else 0, len(lregs)))
    print('  id  %-14s version      length       fields' % 'name')
    for r in lregs:
        print('  %-3d %-14s 0x%08X %-12d %d' %
              (r['id'], r['name'], r['version'], r['len'], len(r['fields'])))

    if not good:
        print('\nverdict: INCOMPATIBLE — the file is corrupt or not an NNKS state')
        return 2
    if lbuild is not None and build != lbuild:
        print('\nnote: the file was written by build 0x%08X, this build is 0x%08X'
              % (build, lbuild))

    # ---- per-region classification ----
    byid = {r['id']: r for r in lregs}
    byname = {r['name']: r for r in lregs}
    seen = set()
    exact = partial = 0
    lines = []
    for r in regs:
        lr = byid.get(r['id']) if r['id'] else byname.get(r['name'])
        if lr is None:
            lines.append('  %-14s DROPPED  (not in this build)' % r['name'])
            partial += 1
            continue
        seen.add(lr['id'])
        if (r['version'] == lr['version'] and r['len'] == lr['len']):
            lines.append('  %-14s EXACT    %d B (v0x%08X)' % (r['name'], r['len'], r['version']))
            exact += 1
            continue
        partial += 1
        detail = '  %-14s PARTIAL  stored %d B (v0x%08X) -> build %d B (v0x%08X)' % (
            r['name'], r['len'], r['version'], lr['len'], lr['version'])
        lines.append(detail)
        if r['dir'] is not None:
            have = {n for (n, _o, _l) in r['dir']}
            now = {n for (n, _o, _l) in lr['fields']}
            new = sorted(now - have)
            gone = sorted(have - now)
            if new:
                lines.append('      %d field(s) DEFAULTED (added since the state): %s'
                             % (len(new), ', '.join(new[:12]) + ('' if len(new) <= 12 else
                                                                ' (+%d more)' % (len(new) - 12))))
            if gone:
                lines.append('      %d stored field(s) no longer in this build: %s'
                             % (len(gone), ', '.join(gone[:12])))
        elif lr['fields']:
            # v1: no directory — a field is covered when its offset+len is inside
            # the stored bytes
            cut = [n for (n, o, l) in lr['fields'] if o + l > r['len']]
            lines.append('      %d field(s) DEFAULTED (past the stored %d B): %s'
                         % (len(cut), r['len'],
                            ', '.join(cut[:12]) + ('' if len(cut) <= 12 else
                                                   ' (+%d more)' % (len(cut) - 12))))
    for lr in lregs:
        if lr['id'] not in seen:
            lines.append('  %-14s MISSING  not in the file -> defaulted' % lr['name'])
            partial += 1
    print('\nper-region:')
    for l in lines:
        print(l)

    print('')
    if partial:
        print('verdict: PARTIAL — %d region(s) exact, %d need a best-effort restore'
              % (exact, partial))
        print('  this build LOADS the file and reports every defaulted field;'
              ' step/frame resume at the saved values')
        print('  NNK_SAVESTATE_STRICT=1 would REFUSE it (layout differs)')
        return 1
    print('verdict: EXACT — every region matches this build: byte-for-byte restore')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
