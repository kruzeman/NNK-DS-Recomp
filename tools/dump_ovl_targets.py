#!/usr/bin/env python3
"""Per-load NITRO overlay target images (Phase-4G).

The NNK port of the DQ8 "dump_ovl_targets" idea: instead of a single
snapshot of the whole overlay window taken at one arbitrary moment, produce
ONE image per *load generation* of every overlay, i.e. the bytes that
overlay places at its OVT `ramAddress` when it is the module resident in
that load arena.

Source of truth is the ROM itself: the ARM9 overlay table (header 0x50),
the FAT, and Nintendo code-compression.  Nothing from a RAM snapshot is
used, so the images are the exact bytes the guest loader computes.

Because every overlay's `ramAddress`/`ramSize` is known, the tool also
derives the *arena* structure (several overlays share one load arena) and
the address ranges where two overlays disagree -- the collisions a
one-case-per-PC dispatch image cannot represent without a resident-module
(generation) check.

Outputs (default `work/ovl_targets/`):
  ovl_<id>_<ram>.bin    decompressed slot image, ramSize bytes, zero padded
  registry.tsv          id file_id ram ram_size bss sinit_start sha1
  arenas.tsv            arena_base  ids  max_size
  conflicts.tsv         lo hi ids...   (4-byte granularity, coalesced runs)
  ovt_stored.tsv        id file_id rom_start rom_end stored_size flags

usage: tools/dump_ovl_targets.py <rom> [out_dir] [--quiet]
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.expanduser("~/.local/lib/python3.12/site-packages"))
from ndspy import codeCompression  # noqa: E402


def parse_rom(rom):
    arm9_off, arm9_entry, arm9_ram, arm9_size = struct.unpack_from("<IIII", rom, 0x20)
    fat_off, fat_size = struct.unpack_from("<II", rom, 0x48)
    ovt_off, ovt_size = struct.unpack_from("<II", rom, 0x50)
    n = ovt_size // 32
    ovt = []
    for i in range(n):
        # NITRO 32-byte record: overlayId, ramAddress, ramSize, bssSize,
        # staticInitStart, staticInitEnd, fileId, compressedSize|flags<<24
        (oid, ram, ram_size, bss, sinit_b, sinit_e, file_id, comp_field) = struct.unpack_from(
            "<IIIIIIII", rom, ovt_off + 32 * i)
        flags = (comp_field >> 24) & 0xFF
        comp = comp_field & 0xFFFFFF
        start = struct.unpack_from("<I", rom, fat_off + 8 * file_id)[0]
        end = struct.unpack_from("<I", rom, fat_off + 8 * file_id + 4)[0]
        ovt.append(dict(id=oid, file_id=file_id, ram=ram, ram_size=ram_size, bss=bss,
                        sinit_b=sinit_b, sinit_e=sinit_e, flags=flags,
                        rom_start=start, rom_end=end, comp=comp))
    return arm9_off, ovt


def slot_image(rom, o):
    stored = rom[o["rom_start"]:o["rom_end"]]
    if o["flags"] & 1:
        img = codeCompression.decompress(stored)
    else:
        img = stored
    if len(img) < o["ram_size"]:
        img = img + bytes(o["ram_size"] - len(img))
    return img[:o["ram_size"]]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    if not args:
        sys.exit(__doc__)
    rom_path = args[0]
    out = args[1] if len(args) > 1 else "work/ovl_targets"
    rom = open(rom_path, "rb").read()
    os.makedirs(out, exist_ok=True)
    _arm9, ovt = parse_rom(rom)

    reg = ["# id\tfile_id\tram\tram_size\tbss\tsinit_start\tsinit_end\tsha1"]
    stored = ["# id\tfile_id\trom_start\trom_end\tstored\tflags\tcomp"]
    images = {}
    for o in ovt:
        img = slot_image(rom, o)
        images[o["id"]] = img
        name = os.path.join(out, "ovl_%02d_%08X.bin" % (o["id"], o["ram"]))
        open(name, "wb").write(img)
        reg.append("%d\t%d\t0x%08X\t0x%X\t0x%X\t0x%08X\t0x%08X\t%s" % (
            o["id"], o["file_id"], o["ram"], o["ram_size"], o["bss"],
            o["sinit_b"], o["sinit_e"], hashlib.sha1(img).hexdigest()))
        stored.append("%d\t%d\t0x%08X\t0x%08X\t0x%X\t0x%02X\t0x%X" % (
            o["id"], o["file_id"], o["rom_start"], o["rom_end"],
            o["rom_end"] - o["rom_start"], o["flags"], o["comp"]))
    open(os.path.join(out, "registry.tsv"), "w").write("\n".join(reg) + "\n")
    open(os.path.join(out, "ovt_stored.tsv"), "w").write("\n".join(stored) + "\n")

    # arenas: overlays sharing one ramAddress
    arenas = {}
    for o in ovt:
        arenas.setdefault(o["ram"], []).append(o)
    ar = ["# arena_base\tids\tspan"]
    for base in sorted(arenas):
        ids = [o["id"] for o in arenas[base]]
        span = max(o["ram_size"] for o in arenas[base])
        ar.append("0x%08X\t%s\t0x%X" % (base, ",".join(str(i) for i in ids), span))
    open(os.path.join(out, "arenas.tsv"), "w").write("\n".join(ar) + "\n")

    # conflicts: 4-byte words where two resident candidates disagree
    cf = ["# lo\thi\tids\tdiffering_bytes"]
    for base in sorted(arenas):
        cands = arenas[base]
        span = max(o["ram_size"] for o in cands)
        n_word = (span + 3) // 4
        run_start = None
        for wi in range(n_word):
            off = wi * 4
            vals = set()
            ids = []
            for o in cands:
                if off + 4 <= o["ram_size"]:
                    w = images[o["id"]][off:off + 4]
                    if w != b"\0\0\0\0":
                        vals.add(w)
                        ids.append(o["id"])
            conflict = len(vals) > 1
            if conflict and run_start is None:
                run_start = off
            elif not conflict and run_start is not None:
                cf.append("0x%08X\t0x%08X\t%s\t%d" % (
                    base + run_start, base + off, ",".join(str(i) for i in ids),
                    off - run_start))
                run_start = None
        if run_start is not None:
            cf.append("0x%08X\t0x%08X\t%s\t%d" % (
                base + run_start, base + span, ",".join(str(i) for i in cands), span - run_start))
    open(os.path.join(out, "conflicts.tsv"), "w").write("\n".join(cf) + "\n")

    if not quiet:
        print("ovl target images -> %s" % out)
        for base in sorted(arenas):
            print("  arena 0x%08X ids %s span 0x%X" % (
                base, ",".join(str(o["id"]) for o in arenas[base]),
                max(o["ram_size"] for o in arenas[base])))
        print("  conflicts: %d run(s) -> %s/conflicts.tsv" % (len(cf) - 1, out))


if __name__ == "__main__":
    main()
