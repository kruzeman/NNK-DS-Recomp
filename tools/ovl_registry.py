#!/usr/bin/env python3
"""Runtime overlay-slot registry (Phase-4G, DQ8 ovl_registry pattern).

One registry of overlay-to-slot state, shared by the build tooling and the
runtime trace, so "which module is resident in which arena, and which load
generation is it" is answered from data instead of from a guess.

Subcommands
  build   --rom ROM --out DIR [--trace FILE]
      Build the per-load generation table.  --trace is the runtime's
      NNK_OVL_TRACE log (`ovl_load rom=... size=... id=N dest=0x...`), i.e.
      the load events the guest actually performed; each event appends a
      generation that makes overlay N resident in arena `dest`.  Without a
      trace only the ROM-derived arena/conflict structure is produced.

  match   --dump FILE --images DIR
      Identify the resident overlay of each arena from memory: FILE is a
      melonDS RegProbe hexdump (`range <addr> len <n>` + `0xADDR: ..`), e.g.
      MELONDS_REGPROBE_MEM="0x02159FE0:0x2200" at a probe hit.  Every arena
      that overlaps the dump is scored against each of its candidate overlay
      images and the best match is reported -- this is the ground-truth check
      that a per-load dump describes the module the guest is really running.

  conflicts --images DIR [--output F]
      Rewrite/print the address runs where two overlays disagree (the
      addresses a one-case-per-PC image cannot represent without a
      resident-module check).

  resident --images DIR --arena ADDR --id N [--lo A --hi B]
      Print the byte ranges inside an arena that overlay N owns (non-zero)
      and the ranges that overlap another candidate (collisions), i.e. what
      a generation-aware dispatch would have to cover.

`build` writes generations.tsv (gen, id, dest, rom, size) and
resident.tsv (gen, arena, ids) next to the images.
"""
import os
import struct
import sys


def load_registry(images_dir):
    reg = []
    path = os.path.join(images_dir, "registry.tsv")
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        reg.append(dict(id=int(f[0]), file_id=int(f[1]), ram=int(f[2], 16),
                        ram_size=int(f[3], 16), bss=int(f[4], 16),
                        sinit_b=int(f[5], 16), sinit_e=int(f[6], 16), sha1=f[7]))
    for r in reg:
        name = "ovl_%02d_%08X.bin" % (r["id"], r["ram"])
        r["image"] = open(os.path.join(images_dir, name), "rb").read()
    return reg


def arenas(reg):
    a = {}
    for r in reg:
        a.setdefault(r["ram"], []).append(r)
    return a


def stored_ranges(images_dir):
    """overlay-id lookup by (rom_start, stored size) -- the card-level identity
    of a load event."""
    out = {}
    for line in open(os.path.join(images_dir, "ovt_stored.tsv")):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        out[(int(f[2], 16), int(f[4], 16))] = int(f[0])
    return out


def parse_probe_dump(path):
    """melonDS RegProbe MEM dump -> list of (addr, bytes) ranges."""
    ranges = []
    cur = None
    for line in open(path, errors="replace"):
        line = line.strip()
        if line.startswith("range "):
            _, a, _, n = line.split()
            cur = [int(a, 16), int(n, 16), bytearray(int(n, 16)), set()]
            ranges.append(cur)
        elif line.startswith("0x") and cur is not None and ":" in line:
            a_s, body = line.split(":", 1)
            a = int(a_s, 16)
            data = bytes(int(x, 16) for x in body.split())
            off = a - cur[0]
            if 0 <= off < cur[1]:
                cur[2][off:off + len(data)] = data
                cur[3].add(a)
    return [(a, n, bytes(b)) for a, n, b, _ in ranges]


def score(image, off, data):
    """fraction of non-zero data bytes that agree, plus agreement on zeros."""
    n = min(len(data), len(image) - off)
    if n <= 0:
        return None
    same = nz = 0
    for i in range(n):
        d = data[i]
        if d:
            nz += 1
            if d == image[off + i]:
                same += 1
    if nz == 0:
        return (0.0, 0)
    return (same / nz, nz)


def cmd_match(args):
    dump = args["dump"]
    images_dir = args["images"]
    reg = load_registry(images_dir)
    arenas_ = arenas(reg)
    ranges = parse_probe_dump(dump)
    if not ranges:
        sys.exit("no ranges parsed from %s" % dump)
    for base, n, data in ranges:
        hi = base + n
        print("range 0x%08X..0x%08X (%d bytes)" % (base, hi, n))
        for abase in sorted(arenas_):
            cands = arenas_[abase]
            span = max(c["ram_size"] for c in cands)
            if abase >= hi or abase + span <= base:
                continue
            off = base - abase
            sub = data
            if off < 0:
                sub = data[-off:]
                off = 0
            print("  arena 0x%08X candidates %s" % (
                abase, ",".join(str(c["id"]) for c in cands)))
            best = None
            for c in cands:
                s = score(c["image"], off, sub)
                if s is None:
                    continue
                print("    ovl %2d: %.4f%% of %d non-zero bytes match" % (
                    c["id"], s[0] * 100.0, s[1]))
                if best is None or s[0] > best[1]:
                    best = (c["id"], s[0], s[1])
            if best:
                print("    -> resident: overlay %d (%.2f%%)" % (best[0], best[1] * 100.0))


def cmd_conflicts(args):
    images_dir = args["images"]
    reg = load_registry(images_dir)
    arenas_ = arenas(reg)
    total = 0
    for abase in sorted(arenas_):
        cands = arenas_[abase]
        span = max(c["ram_size"] for c in cands)
        runs = []
        start = None
        for off in range(0, span, 4):
            vals = set()
            ids = []
            for c in cands:
                if off + 4 <= c["ram_size"]:
                    w = c["image"][off:off + 4]
                    if w != b"\0\0\0\0":
                        vals.add(w)
                        ids.append(c["id"])
            if len(vals) > 1:
                if start is None:
                    start = off
            else:
                if start is not None:
                    runs.append((start, off, ids))
                    start = None
        if start is not None:
            runs.append((start, span, ids))
        if runs:
            total += 1
            print("arena 0x%08X: %d conflicting run(s), %d bytes" % (
                abase, len(runs), sum(b - a for a, b, _ in runs)))
            for a, b, ids in runs[:6]:
                print("   0x%08X..0x%08X ids %s" % (abase + a, abase + b,
                                                   ",".join(str(i) for i in ids)))
            if len(runs) > 6:
                print("   ... %d more" % (len(runs) - 6))
    if not total:
        print("no conflicts")


def cmd_build(args):
    images_dir = args["images"]
    reg = load_registry(images_dir)
    arenas_ = arenas(reg)
    by_rom = stored_ranges(images_dir)
    by_id = {r["id"]: r for r in reg}
    gens = []
    trace = args.get("trace")
    if trace:
        for line in open(trace):
            line = line.strip()
            if not line.startswith("ovl_load"):
                continue
            kv = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
            rid = int(kv["id"])
            dest = int(kv["dest"], 16)
            gens.append((len(gens) + 1, rid, dest, int(kv["rom"], 16), int(kv["size"], 16)))
    if not gens:
        # No runtime trace yet: assume the boot order (overlay id order), which
        # is what a fresh boot with no reloads performs.
        for r in reg:
            gens.append((len(gens) + 1, r["id"], r["ram"], 0, r["ram_size"]))
    g = ["# gen\tid\tdest\trom\tsize"]
    for gen, rid, dest, rom, size in gens:
        g.append("%d\t%d\t0x%08X\t0x%08X\t0x%X" % (gen, rid, dest, rom, size))
    open(os.path.join(images_dir, "generations.tsv"), "w").write("\n".join(g) + "\n")

    resident = {}
    rs = ["# gen\tid\tarena\tids_resident"]
    for gen, rid, dest, _rom, _size in gens:
        resident.setdefault(dest, set()).add(rid)
        rs.append("%d\t%d\t0x%08X\t%s" % (gen, rid, dest,
                                          ",".join(str(i) for i in sorted(resident[dest]))))
    open(os.path.join(images_dir, "resident.tsv"), "w").write("\n".join(rs) + "\n")

    print("generations: %d (last overlay: %d)" % (len(gens), gens[-1][1] if gens else -1))
    for abase in sorted(arenas_):
        seen = sorted({rid for _g, rid, dest, _r, _s in gens if dest == abase})
        print("  arena 0x%08X: loads of ids %s (candidates %s)" % (
            abase, ",".join(str(i) for i in seen) or "-",
            ",".join(str(c["id"]) for c in arenas_[abase])))
    del by_rom, by_id


def cmd_resident(args):
    images_dir = args["images"]
    arena = int(args["arena"], 16)
    rid = int(args["id"])
    reg = load_registry(images_dir)
    arenas_ = arenas(reg)
    cands = arenas_.get(arena)
    if not cands:
        sys.exit("no arena at 0x%08X" % arena)
    img = [c for c in cands if c["id"] == rid]
    if not img:
        sys.exit("overlay %d is not a candidate of arena 0x%08X" % (rid, arena))
    img = img[0]["image"]
    span = max(c["ram_size"] for c in cands)
    runs = []
    start = None
    for off in range(0, span, 4):
        owned = off + 4 <= len(img) and img[off:off + 4] != b"\0\0\0\0"
        if owned and start is None:
            start = off
        elif not owned and start is not None:
            runs.append((start, off))
            start = None
    if start is not None:
        runs.append((start, min(span, len(img))))
    print("overlay %d resident in arena 0x%08X: %d owned run(s), %d bytes" % (
        rid, arena, len(runs), sum(b - a for a, b in runs)))
    for a, b in runs:
        print("   0x%08X..0x%08X (0x%X)" % (arena + a, arena + b, b - a))


def cmd_slots(args):
    """Write the raw captured bytes of one arena window to a file.

    This is the per-load slot DUMP half of the Phase-4G tooling: the capture
    (a melonDS RegProbe MEM dump taken at the moment the resident module is
    entered) already contains every page the guest decrypted and initialised,
    which the ROM image alone cannot express.
    """
    dump = args["dump"]
    out = args["out"]
    arena = int(args["arena"], 16) if "arena" in args else None
    size = int(args["size"], 16) if "size" in args else None
    want = [(arena, size)] if arena is not None else None
    got = {}
    for base, n, data in parse_probe_dump(dump):
        if want:
            for a, s in want:
                lo, hi = max(base, a), min(base + n, a + s)
                if lo < hi:
                    buf = got.setdefault(a, bytearray(s))
                    buf[lo - a:hi - a] = data[lo - base:hi - base]
        else:
            got.setdefault(base, bytearray(n))
            got[base][:n] = data
    if not got:
        sys.exit("no captured bytes for arena %s in %s" % (
            ("0x%08X" % arena) if arena else "(any)", dump))
    for a in sorted(got):
        open(out, "wb").write(bytes(got[a]))
        print("wrote %s: 0x%X bytes of arena 0x%08X (%d non-zero)" % (
            out, len(got[a]), a, sum(1 for b in got[a] if b)))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    args = {}
    i = 2
    while i < len(sys.argv):
        a = sys.argv[i]
        if a.startswith("--"):
            args[a[2:]] = sys.argv[i + 1]
            i += 2
        else:
            i += 1
    args.setdefault("images", "work/ovl_targets")
    {"match": cmd_match, "conflicts": cmd_conflicts, "build": cmd_build,
     "resident": cmd_resident, "slots": cmd_slots}[cmd](args)


if __name__ == "__main__":
    main()
