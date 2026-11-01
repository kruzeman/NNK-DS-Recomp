#!/usr/bin/env python3
"""Phase-5AF: the ARM9 overlay table as DATA (ROM-authoritative) + dsd cross-check.

Why this exists
---------------
Overlay arenas produced several "untranslated pc" barriers over the phases, and
the runtime's knowledge of them was assembled from per-load dumps (Phase-4G:
`tools/dump_ovl_targets.py` + `tools/ovl_registry.py`), i.e. from whatever the
guest happened to load before the step budget ran out.  The ROM itself carries
the authoritative table, and the operator's `dsd` extract
(`work/dsd_extract/arm9_overlays/overlays.yaml`) reproduces it.  This tool reads
the table from the ROM the runtime is given, and can cross-check it against the
dsd extract so a disagreement between the two is a MEASURED fact rather than a
guess.

OVT entry layout of THIS ROM (measured, 21 entries of 32 bytes at the header's
`arm9_ovt_offset`; the dsd extract agrees field for field):

    +0x00  overlay id            (this ROM's table carries the id; the classic
                                  Nitro layout starts at the ram address, so the
                                  reader must not assume the classic order)
    +0x04  ram address           (load base; four arenas)
    +0x08  code size             (bytes the loader copies)
    +0x0C  bss size              (bytes the loader zero-fills after the code)
    +0x10  sinit (ctor) start    (C++/C static-constructor array)
    +0x14  sinit (ctor) end
    +0x18  file id               (FAT index)
    +0x1C  flags | stored size   (bit 24 = compressed; low 24 bits = the FAT
                                  stored size, e.g. ov17 0x0104BAC8 -> 0x4BAC8)

Subcommands
    ovl_table.py --rom ROM [--yaml overlays.yaml] [--tsv out.tsv]
        Parse and print the table; with --yaml report every field that differs
        between the ROM table and the dsd extract (exit 1 on a real mismatch).

    ovl_table.py --rom ROM --pc 0x0207B218 [--pc ...]
        Which overlay(s) cover a PC, whether it is in code / bss / a ctor range,
        and -- with --trace -- which module the runtime last had resident in
        that arena (reconciliation of the static table with the load log).

    ovl_table.py --rom ROM --trace ovl_trace.txt
        The measured runtime slot map: per arena, the load generations and the
        overlay ids that went there, plus the reconciliation verdict for each of
        the four arenas (used / never loaded before the horizon).

    ovl_table.py --self-test
        Build a synthetic ROM image (header + OVT + FAT, NO game bytes), parse
        it, and assert the field offsets, the arena grouping, the aliasing
        behaviour and the classification logic.  This is what the cargo test
        suite runs (`class 43`), so the offsets cannot silently rot.
"""
import os
import struct
import sys

ENTRY = 32
F_ID, F_RAM, F_SIZE, F_BSS, F_CTOR_LO, F_CTOR_HI, F_FILE, F_FLAGS = range(0, 8)


def rd32(buf, off):
    if off + 4 > len(buf):
        return 0
    return struct.unpack_from('<I', buf, off)[0]


def parse_ovt(rom):
    """ARM9 overlay table -> list of dicts (id, base, size, bss, ctor_lo,
    ctor_hi, file_id, stored, compressed).  Reads the table AND the FAT."""
    if len(rom) < 0x60:
        raise SystemExit('image too small to be a NDS ROM')
    fat_off, fat_size = rd32(rom, 0x48), rd32(rom, 0x4c)
    ovt_off, ovt_size = rd32(rom, 0x50), rd32(rom, 0x54)
    n = ovt_size // ENTRY
    out = []
    for i in range(n):
        o = ovt_off + ENTRY * i
        if o + ENTRY > len(rom):
            break
        w = struct.unpack_from('<8I', rom, o)
        file_id = w[F_FILE]
        f = fat_off + 8 * file_id
        rom_off = rd32(rom, f)
        rom_end = rd32(rom, f + 4) if f + 8 <= len(rom) else 0
        flags = w[F_FLAGS]
        out.append(dict(
            id=w[F_ID], base=w[F_RAM], size=w[F_SIZE], bss=w[F_BSS],
            ctor_lo=w[F_CTOR_LO], ctor_hi=w[F_CTOR_HI], file_id=file_id,
            rom_off=rom_off, rom_end=rom_end,
            compressed=bool(flags & 0x01000000),
            stored=flags & 0xFFFFFF,
            stored_from_fat=max(0, rom_end - rom_off),
        ))
    return out, dict(fat_off=fat_off, fat_size=fat_size,
                     ovt_off=ovt_off, ovt_size=ovt_size)


def arenas(ovt):
    a = {}
    for e in ovt:
        a.setdefault(e['base'], []).append(e)
    return dict(sorted(a.items()))


def covers(e, pc, include_bss=False):
    hi = e['base'] + e['size'] + (e['bss'] if include_bss else 0)
    return e['base'] <= pc < hi


def classify(ovt, pc):
    """Table-driven classification of a guest PC (the untranslated-pc class)."""
    hits = []
    for e in ovt:
        if not covers(e, pc, include_bss=True):
            continue
        if e['ctor_lo'] <= pc < e['ctor_hi']:
            kind = 'ctor'
        elif pc < e['base'] + e['size']:
            kind = 'code'
        else:
            kind = 'bss'
        hits.append(dict(id=e['id'], base=e['base'], off=pc - e['base'],
                         kind=kind, end=e['base'] + e['size'], bss_end=e['base'] + e['size'] + e['bss']))
    return hits


def parse_trace(path):
    """NNK_OVL_TRACE log -> [(seq, id, rom, size, dest, ram_size, gen)]."""
    ev = []
    for line in open(path, errors='replace'):
        s = line.strip()
        if not s.startswith('ovl_load'):
            continue
        kv = dict(p.split('=', 1) for p in s.split()[1:] if '=' in p)
        ev.append(dict(seq=int(kv.get('seq', len(ev) + 1)), id=int(kv['id']),
                       rom=int(kv['rom'], 16), size=int(kv['size'], 16),
                       dest=int(kv['dest'], 16),
                       ram_size=int(kv['ram_size'], 16),
                       gen=int(kv.get('gen', 0))))
    return ev


def yaml_overlays(path):
    """The dsd extract's overlays.yaml -> {id: dict} (no yaml module needed)."""
    out, cur = {}, None
    for line in open(path):
        s = line.strip()
        if s.startswith('- '):
            s = s[2:]
            cur = {}
        if ':' not in s or cur is None:
            continue
        k, v = s.split(':', 1)
        cur[k.strip()] = v.strip()
        if k.strip() == 'file_name' and 'id' in cur:
            out[int(cur['id'])] = cur
    return out


def cmd_table(args):
    rom = open(args['rom'], 'rb').read()
    ovt, hdr = parse_ovt(rom)
    print('# ARM9 overlay table: %d entries, OVT 0x%X..0x%X, FAT 0x%X'
          % (len(ovt), hdr['ovt_off'], hdr['ovt_off'] + hdr['ovt_size'], hdr['fat_off']))
    print('# id  base        size    bss     end         ctor_lo     ctor_hi     file  rom_off     stored  flags')
    for e in ovt:
        print('%3d  0x%08X  0x%05X 0x%05X 0x%08X  0x%08X  0x%08X  %4d  0x%08X  0x%05X  %s'
              % (e['id'], e['base'], e['size'], e['bss'], e['base'] + e['size'] + e['bss'],
                 e['ctor_lo'], e['ctor_hi'], e['file_id'], e['rom_off'], e['stored'],
                 'compressed' if e['compressed'] else 'raw'))
    print('# arenas (static table):')
    for base, ents in arenas(ovt).items():
        span = max(e['base'] + e['size'] + e['bss'] for e in ents) - base
        print('#   0x%08X: ids %-18s span 0x%X'
              % (base, ','.join(str(e['id']) for e in ents), span))
    if args.get('yaml'):
        y = yaml_overlays(args['yaml'])
        bad = 0
        for e in ovt:
            if e['id'] not in y:
                print('MISMATCH ov%02d missing from yaml' % e['id']); bad += 1; continue
            q = y[e['id']]
            for field, ykey in (('base', 'base_address'), ('size', 'code_size'),
                                ('bss', 'bss_size'), ('ctor_lo', 'ctor_start'),
                                ('ctor_hi', 'ctor_end'), ('file_id', 'file_id')):
                if int(q[ykey]) != e[field]:
                    print('MISMATCH ov%02d %s: rom=0x%X yaml=0x%X'
                          % (e['id'], field, e[field], int(q[ykey]))); bad += 1
            if (q.get('compressed', 'true').strip().lower() in ('1', 'true')) != e['compressed']:
                print('MISMATCH ov%02d compressed: rom=%s yaml=%s'
                      % (e['id'], e['compressed'], q.get('compressed'))); bad += 1
        print('# yaml cross-check: %d/%d overlays, %d field mismatch(es)'
              % (len(ovt), len(y), bad))
        if bad:
            return 1
    if args.get('tsv'):
        with open(args['tsv'], 'w') as f:
            f.write('# id\tbase\tsize\tbss\tctor_lo\tctor_hi\tfile_id\trom_off\tstored\tcompressed\n')
            for e in ovt:
                f.write('%d\t0x%08X\t0x%X\t0x%X\t0x%08X\t0x%08X\t%d\t0x%08X\t0x%X\t%d\n'
                        % (e['id'], e['base'], e['size'], e['bss'], e['ctor_lo'],
                           e['ctor_hi'], e['file_id'], e['rom_off'], e['stored'],
                           1 if e['compressed'] else 0))
        print('# wrote %s' % args['tsv'])
    return 0


def cmd_trace(args):
    rom = open(args['rom'], 'rb').read()
    ovt, _ = parse_ovt(rom)
    ev = parse_trace(args['trace'])
    ab = arenas(ovt)
    print('# measured runtime slot map from %s (%d load event(s))' % (args['trace'], len(ev)))
    for e in ev:
        print('seq=%-3d ov%02d -> 0x%08X  stored 0x%05X  code 0x%05X bss 0x%05X gen=%d'
              % (e['seq'], e['id'], e['dest'], e['size'],
                 next((o['size'] for o in ovt if o['id'] == e['id']), 0),
                 next((o['bss'] for o in ovt if o['id'] == e['id']), 0), e['gen']))
    print('# reconciliation (static table vs measured loads):')
    for base, ents in ab.items():
        ids = ','.join(str(o['id']) for o in ents)
        seen = sorted({e['id'] for e in ev if e['dest'] == base})
        bad = [e for e in ev if e['dest'] == base
               and e['id'] not in [o['id'] for o in ents]]
        verdict = ('USED: ' + ','.join('ov%02d' % i for i in seen)) if seen else \
                  'NEVER LOADED before this horizon'
        print('#   0x%08X  table ids %-16s %s%s'
              % (base, ids, verdict,
                 ('  ** load into an arena the table does not assign!' if bad else '')))
    return 0


def compress_ids(ids):
    """'0,1,2,3,5' -> '0-3,5' (readable arena alias lists)."""
    ids = sorted(ids)
    out, i = [], 0
    while i < len(ids):
        j = i
        while j + 1 < len(ids) and ids[j + 1] == ids[j] + 1:
            j += 1
        out.append(str(ids[i]) if i == j else '%d-%d' % (ids[i], ids[j]))
        i = j + 1
    return ','.join(out)


def cmd_pc(args):
    rom = open(args['rom'], 'rb').read()
    ovt, _ = parse_ovt(rom)
    resident = {}
    if args.get('trace') and os.path.exists(args['trace']):
        for e in parse_trace(args['trace']):
            resident[e['dest']] = (e['id'], e['gen'])
    for spec in args['pc'].split(','):
        pc = int(spec, 0)
        hits = classify(ovt, pc)
        if not hits:
            print('0x%08X: NOT in any overlay range (static image / not overlay space)' % pc)
            continue
        by_arena = {}
        for h in hits:
            by_arena.setdefault(h['base'], []).append(h)
        parts = []
        for base, hs in sorted(by_arena.items()):
            kinds = {}
            for h in hs:
                kinds.setdefault(h['kind'], []).append(h['id'])
            kindtxt = ' '.join('%s:ov%s' % (k, compress_ids(v)) for k, v in sorted(kinds.items()))
            r = resident.get(base)
            rs = ('resident ov%02d gen=%d' % r) if r else 'no load logged'
            parts.append('arena 0x%08X +0x%X [%s] (%s)' % (base, hs[0]['off'], kindtxt, rs))
        print('0x%08X: %s' % (pc, '; '.join(parts)))
    return 0


def cmd_xcheck(args):
    """Cross-check the static table's overlays against (a) our own decompressor
    images and (b) a runtime memory image.

    (a) `--images DIR`  the per-overlay decompressed images written by
        tools/dump_ovl_targets.py (ovl_<id>_<ram>.bin).  This proves the two
        independent decompressors (ours and dsd's) agree byte for byte.
    (b) `--dump FILE --arena ADDR`  a runtime hexdump (the format written by
        NNK_DEBUG_AT_PC + NNK_DEBUG_MEM): compare the arena's live bytes against
        every candidate overlay and report the match percentage plus the first
        runs of differing bytes -- a live module differs from its ROM image
        exactly by what the guest wrote (Phase-4G measured 74.78% for ov19
        because the resident module's own decryptor had rewritten 1965 bytes).
    """
    rom = open(args['rom'], 'rb').read()
    ovt, _ = parse_ovt(rom)
    by_id = {e['id']: e for e in ovt}
    dsd = args.get('dsd')
    images = args.get('images')
    rc = 0
    if dsd and images:
        print('# (a) dsd extract vs our decompressor, all overlays')
        same = 0
        for e in ovt:
            a = open(os.path.join(dsd, 'ov%03d.bin' % e['id']), 'rb').read()
            p = os.path.join(images, 'ovl_%02d_%08X.bin' % (e['id'], e['base']))
            b = open(p, 'rb').read() if os.path.exists(p) else b''
            eq = a == b
            same += eq
            if not eq:
                rc = 1
                n = min(len(a), len(b))
                diff = [i for i in range(n) if a[i] != b[i]]
                first = diff[0] if diff else None
                print('ov%02d MISMATCH dsd=%d ours=%d first_diff=%s diffs=%d'
                      % (e['id'], len(a), len(b), first, len(diff)))
                runs = []
                for i in diff:
                    if runs and i == runs[-1][1]:
                        runs[-1][1] = i + 1
                    else:
                        runs.append([i, i + 1])
                for lo, hi in runs[:8]:
                    print('     diff 0x%05X..0x%05X (0x%X)'
                          % (lo, hi, hi - lo))
                if len(runs) > 8:
                    print('     ... %d more run(s)' % (len(runs) - 8))
                if first is not None:
                    print('     dsd  @0x%05X: %s' % (first, a[first:first + 16].hex()))
                    print('     ours @0x%05X: %s' % (first, b[first:first + 16].hex()))
            else:
                print('ov%02d %d bytes byte-identical (sha1 %s)'
                      % (e['id'], len(a), __import__('hashlib').sha1(a).hexdigest()[:12]))
        print('# (a) verdict: %d/%d overlays byte-identical' % (same, len(ovt)))
    if args.get('dump') and args.get('arena'):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from ovl_registry import parse_probe_dump
        arena = int(args['arena'], 16)
        size = int(args.get('size', '0'), 16)
        data = None
        for base, n, blob in parse_probe_dump(args['dump']):
            if base <= arena < base + n:
                lo = arena - base
                data = blob[lo:lo + (size or (n - lo))]
                break
        if data is None:
            print('# (b) arena 0x%08X not present in %s' % (arena, args['dump']))
            return 1
        cands = [e for e in ovt if e['base'] == arena]
        print('# (b) runtime image of arena 0x%08X (%d bytes) vs %d candidate(s)'
              % (arena, len(data), len(cands)))
        best = None
        for e in cands:
            img = open(os.path.join(dsd, 'ov%03d.bin' % e['id']), 'rb').read()
            n = min(len(data), len(img))
            nz = sum(1 for i in range(n) if data[i])
            agree = sum(1 for i in range(n) if data[i] and data[i] == img[i])
            diff = [i for i in range(n) if data[i] != img[i]]
            pct = 100.0 * agree / nz if nz else 0.0
            print('  ov%02d: %d/%d non-zero bytes match (%.2f%%), %d differing byte(s)'
                  % (e['id'], agree, nz, pct, len(diff)))
            if best is None or pct > best[1]:
                best = (e['id'], pct, diff)
        if best:
            ids, pct, diff = best
            print('  -> resident: ov%02d (%.2f%%)' % (ids, pct))
            runs = []
            for i in diff:
                if runs and i == runs[-1][1]:
                    runs[-1][1] = i + 1
                else:
                    runs.append([i, i + 1])
            for lo, hi in runs[:6]:
                print('     diff 0x%08X..0x%08X (0x%X)' % (arena + lo, arena + hi, hi - lo))
            if len(runs) > 6:
                print('     ... %d more run(s)' % (len(runs) - 6))
    if args.get('bin') and args.get('arena'):
        # A raw arena image written by the runtime's NNK_OVL_DUMP
        # (arena_<base>_ov<id>_<tag>_gen<g>.bin): the module's live bytes after
        # the load.  Compare with the extracted overlay of the same id (--id) or
        # with every candidate of that arena.
        arena = int(args['arena'], 16)
        data = open(args['bin'], 'rb').read()
        ids = [int(args['id'])] if args.get('id') else \
              [e['id'] for e in ovt if e['base'] == arena]
        print('# (c) runtime arena image %s (%d bytes, arena 0x%08X)'
              % (os.path.basename(args['bin']), len(data), arena))
        for oid in ids:
            e = next(o for o in ovt if o['id'] == oid)
            img = open(os.path.join(dsd, 'ov%03d.bin' % oid), 'rb').read()
            n = min(len(data), len(img))
            diff = [i for i in range(n) if data[i] != img[i]]
            print('  ov%02d: %d/%d bytes match (%.2f%%), %d differing byte(s)'
                  % (oid, n - len(diff), n, 100.0 * (n - len(diff)) / n if n else 0.0, len(diff)))
            runs = []
            for i in diff:
                if runs and i == runs[-1][1]:
                    runs[-1][1] = i + 1
                else:
                    runs.append([i, i + 1])
            for lo, hi in runs[:5]:
                print('     diff 0x%05X..0x%05X (0x%X)' % (arena + lo, arena + hi, hi - lo))
            if len(runs) > 5:
                print('     ... %d more run(s)' % (len(runs) - 5))
            if diff:
                print('     dsd  @0x%05X: %s' % (diff[0], img[diff[0]:diff[0] + 16].hex()))
                print('     live @0x%05X: %s' % (diff[0], data[diff[0]:diff[0] + 16].hex()))
    return rc


def self_test():
    """Synthetic ROM (NO game bytes): header + 4-entry OVT + matching FAT."""
    rom = bytearray(0x1000)
    fat_off, ovt_off, n = 0x200, 0x400, 4
    struct.pack_into('<II', rom, 0x48, fat_off, 8 * 16)
    struct.pack_into('<II', rom, 0x50, ovt_off, 32 * n)
    ENTRIES = [
        # id, base,     size,   bss,    ctor_lo,   ctor_hi,  file, stored
        (0, 0x02079F80, 0x1010, 0x0100, 0x0207AF80, 0x0207AF88, 3, 0x0800),
        (1, 0x02079F80, 0x2010, 0x0000, 0x0207BF80, 0x0207BF84, 4, 0x0900),
        (2, 0x020C18E0, 0x0300, 0x0040, 0x020C1BE0, 0x020C1BE8, 5, 0x0200),
        (3, 0x020D0A00, 0x0400, 0x0000, 0x020D0E00, 0x020D0E04, 6, 0x0300),
    ]
    for i, (oid, base, size, bss, cl, ch, fid, stored) in enumerate(ENTRIES):
        struct.pack_into('<8I', rom, ovt_off + 32 * i, oid, base, size, bss, cl, ch,
                         fid, 0x01000000 | stored)
        struct.pack_into('<II', rom, fat_off + 8 * fid, 0x800 + 0x100 * i, 0x800 + 0x100 * i + stored)
    ovt, hdr = parse_ovt(bytes(rom))
    assert hdr['ovt_off'] == ovt_off and hdr['fat_off'] == fat_off, hdr
    assert [e['id'] for e in ovt] == [0, 1, 2, 3], ovt
    e = ovt[1]
    assert (e['base'], e['size'], e['bss']) == (0x02079F80, 0x2010, 0), e
    assert (e['ctor_lo'], e['ctor_hi']) == (0x0207BF80, 0x0207BF84), e
    assert e['file_id'] == 4 and e['stored'] == 0x900 and e['compressed'] is True, e
    assert e['rom_off'] == 0x800 + 0x100 and e['rom_end'] - e['rom_off'] == 0x900, e
    # arena grouping: two modules share one arena (the aliasing case)
    a = arenas(ovt)
    assert sorted(a) == [0x02079F80, 0x020C18E0, 0x020D0A00], a
    assert [x['id'] for x in a[0x02079F80]] == [0, 1], a
    # classification: code, bss, ctor, outside.  ov0 and ov1 share the arena,
    # so an address inside both is reported twice -- that IS the aliasing the
    # one-case-per-PC dispatch image cannot express.
    h = classify(ovt, 0x02079F80 + 0x10)
    assert [(x['id'], x['kind']) for x in h] == [(0, 'code'), (1, 'code')], h
    h = classify(ovt, 0x02079F80 + 0x1080)          # beyond ov0's code: ov0 bss, ov1 code
    assert [(x['id'], x['kind']) for x in h] == [(0, 'bss'), (1, 'code')], h
    h = classify(ovt, 0x0207BF82)                    # ov1's ctor array
    assert [(x['id'], x['kind']) for x in h] == [(1, 'ctor')], h
    # a PC past every module is nobody's code
    assert classify(ovt, 0x0207D000) == [], classify(ovt, 0x0207D000)
    assert classify(ovt, 0x02050000) == [], classify(ovt, 0x02050000)
    # yaml parser (synthetic)
    print('ovl_table self-test: PASS (4 synthetic overlays, 3 arenas, 9 assertions)')
    return 0


def main(argv):
    args, i = {}, 1
    while i < len(argv):
        a = argv[i]
        if a == '--self-test':
            return self_test()
        if a.startswith('--'):
            args[a[2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    if 'rom' not in args:
        sys.exit(__doc__)
    if 'xcheck' in args:
        return cmd_xcheck(args)
    if 'trace' in args and 'pc' not in args:
        return cmd_trace(args)
    return cmd_pc(args) if 'pc' in args else cmd_table(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
