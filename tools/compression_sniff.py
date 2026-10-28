#!/usr/bin/env python3
"""
tools/compression_sniff.py — compression-signature sniff of the B2KJ file table.

Walks the FNT/FAT, resolves every file id to a path, and classifies the first
bytes of every FAT entry:

  LZ10   0x10 + plausible 24-bit BE decompressed size  (validated with ndspy.lz10
                                                        when the file is small)
  LZ11   0x11 + plausible 24-bit BE decompressed size
  NPCK   LEVEL-5 pack container magic 'NPCK' (and its byte-swapped 'KPCN'
         variant); payload block 0 (at +0x3C) is additionally sniffed
  others: sadl/smdl/swdl/spdl (sound), TMAP/WMAP/RTFN/NRCS-family, BCA0, NCv2,
          'pac' marker search (explicitly reported even when absent)

Usage:  /usr/bin/python3 tools/compression_sniff.py [rom] [--paths]
        --paths   also list every file id in each *compression* class

Requires: /usr/bin/python3 with ndspy (pip --user).
"""
import struct
import sys
import collections

LZ_MAX = 0x4000000  # 64 MiB sanity cap for a decompressed size


def load(rom_path):
    f = open(rom_path, "rb")
    h = f.read(0x200)
    fnt_off, fnt_sz, fat_off, fat_sz = struct.unpack_from("<IIII", h, 0x40)
    f.seek(fnt_off); fnt = f.read(fnt_sz)
    f.seek(fat_off); fat = f.read(fat_sz)
    return f, fnt, fat, fat_sz


def parse_paths(fnt):
    def parse_dir(d):
        off, first = struct.unpack_from("<IH", fnt, 8 * d)
        p, out, fi = off, [], 0
        while fnt[p] != 0:
            ctrl = fnt[p]; ln = ctrl & 0x7F
            name = fnt[p + 1:p + 1 + ln].decode("latin-1")
            p += 1 + ln
            if ctrl & 0x80:
                did, = struct.unpack_from("<H", fnt, p); p += 2
                out.append(("D", name, did & 0xFFF))
            else:
                out.append(("F", name, first + fi)); fi += 1
        return out

    paths = {}
    def walk(d, prefix):
        for kind, name, ident in parse_dir(d):
            if kind == "D":
                walk(ident, prefix + name + "/")
            else:
                paths[ident] = prefix + name
    walk(0, "")
    return paths


def classify(head):
    if len(head) < 4:
        return "empty"
    if head[:4] in (b"NPCK", b"KPCN"):
        return head[:4].decode()
    if head[:4].lower()[:3] == b"pac":
        return "pac"
    if head[0] == 0x10:
        sz = int.from_bytes(head[1:4], "big")
        if 0 < sz <= LZ_MAX:
            return "LZ10"
    if head[0] == 0x11:
        sz = int.from_bytes(head[1:4], "big")
        if 0 < sz <= LZ_MAX:
            return "LZ11"
    for mag in (b"sadl", b"smdl", b"swdl", b"spdl", b"TMAP", b"WMAP",
                b"RTFN", b"BCA0", b"NCv2", b"MODS", b"NCV2", b"BMD0"):
        if head.startswith(mag):
            return mag.decode()
    return "other"


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else "Ni No Kuni.nds"
    show_paths = "--paths" in sys.argv

    f, fnt, fat, fat_sz = load(rom)
    paths = parse_paths(fnt)
    fat_n = fat_sz // 8
    print("ROM: %s" % rom)
    print("FAT entries: %d   FNT paths resolved: %d" % (fat_n, len(paths)))
    print()

    classes = collections.Counter()
    exts = collections.defaultdict(collections.Counter)
    examples = {}
    npck_payload = collections.Counter()
    lz10_ids, lz11_ids = [], []
    pac_ids = []

    for fid in range(fat_n):
        s, e = struct.unpack_from("<II", fat, fid * 8)
        f.seek(s)
        head = f.read(16)
        c = classify(head)
        classes[c] += 1
        p = paths.get(fid, "<system-file id=%d>" % fid)
        if "." in p.rsplit("/", 1)[-1]:
            ext = p.rsplit(".", 1)[-1].lower()
        else:
            ext = "<none>"
        exts[ext][c] += 1
        examples.setdefault(c, []).append((fid, p, e - s, head))
        if c == "LZ10":
            lz10_ids.append((fid, p, e - s))
        elif c == "LZ11":
            lz11_ids.append((fid, p, e - s))
        elif c == "pac":
            pac_ids.append((fid, p))
        if head[:4] == b"NPCK" and e - s >= 0x40:
            f.seek(s + 0x3C)
            b = f.read(1)
            npck_payload[("0x%02X" % b[0]) if b else "?"] += 1

    print("== signature classes (first 4 bytes of every FAT entry) ==")
    for c, n in classes.most_common():
        print("  %-10s %6d  (%.1f%%)" % (c, n, 100.0 * n / fat_n))
    print()
    print("== 'pac' marker ==")
    print("  files whose first bytes contain 'pac': %d" % len(pac_ids))
    print()
    print("== NPCK (LEVEL-5 pack) payload block 0 (at +0x3C) first byte ==")
    for k, n in npck_payload.most_common(8):
        print("  %-8s %6d" % (k, n))
    print()

    # LZ10 validation with ndspy where the decompressed size is sane
    print("== LZ10 candidates (validated with ndspy.lz10 where feasible) ==")
    if lz10_ids:
        try:
            import ndspy.lz10
            have = True
        except Exception as ex:
            have = False
            print("  ndspy unavailable: %s" % ex)
        for fid, p, size in lz10_ids:
            s, e = struct.unpack_from("<II", fat, fid * 8)
            f.seek(s); data = f.read(e - s)
            claimed = int.from_bytes(data[1:4], "big")
            verdict = "claimed_size=%d" % claimed
            if have and claimed <= 8 * 1024 * 1024 and size < claimed:
                try:
                    out = ndspy.lz10.decompress(data)
                    verdict = "LZ10 VALID -> %d bytes" % len(out)
                except Exception as ex:
                    verdict = "LZ10 REJECTED (%s)" % type(ex).__name__
            else:
                verdict = "implausible (claimed %d vs stored %d) -> false positive" % (claimed, size)
            print("  id=%-6d stored=%-9d %s  %s" % (fid, size, verdict, p))
    else:
        print("  none")
    print()

    # DS "code compression" (reversed LZ77 + footer) used for ARM9/ARM7 and
    # overlays — invisible to a first-byte sniff, so probe the footer instead.
    print("== code compression (ndspy.codeCompression, footer-based reversed LZ77) ==")
    try:
        from ndspy import codeCompression
        have_cc = True
    except Exception as ex:
        have_cc = False
        print("  ndspy unavailable: %s" % ex)
    if have_cc:
        # the 21 FAT entries not referenced by the FNT are arm9/ovl binaries
        f.seek(0x20); a9 = struct.unpack("<IIII", f.read(16))
        f.seek(0x30); a7 = struct.unpack("<IIII", f.read(16))
        f.seek(a9[0]); d9 = f.read(a9[3])
        f.seek(a7[0]); d7 = f.read(a7[3])
        print("  arm9.bin               stored=0x%-8X -> 0x%X  %s" % (
            len(d9), len(codeCompression.decompress(d9)),
            "code-LZ" if len(codeCompression.decompress(d9)) != len(d9) else "raw"))
        print("  arm7.bin               stored=0x%-8X -> 0x%X  %s" % (
            len(d7), len(codeCompression.decompress(d7)),
            "code-LZ" if len(codeCompression.decompress(d7)) != len(d7) else "raw"))
        n_raw = n_cz = 0
        for fid in range(fat_n):
            if fid < 21:  # system files = the ARM9 overlay binaries
                s, e = struct.unpack_from("<II", fat, fid * 8)
                f.seek(s); data = f.read(e - s)
                out = codeCompression.decompress(data)
                if len(out) != len(data):
                    n_cz += 1
                else:
                    n_raw += 1
        print("  system files (FAT ids 0..20, the 21 ARM9 overlays):")
        print("    code-LZ compressed: %d   raw: %d" % (n_cz, n_raw))
    print()

    print("== LZ11 candidates (0x11 + plausible 24-bit size): %d files ==" % len(lz11_ids))
    by_ext = collections.Counter(p.rsplit(".", 1)[-1].lower() if "." in p.rsplit("/", 1)[-1] else "<none>"
                                 for _, p, _ in lz11_ids)
    for ext, n in by_ext.most_common(10):
        print("  .%-8s %d" % (ext, n))
    for fid, p, size in lz11_ids[:8]:
        s, e = struct.unpack_from("<II", fat, fid * 8)
        f.seek(s); data = f.read(4)
        claimed = int.from_bytes(data[1:4], "big")
        print("  id=%-6d stored=%-9d claimed=%-9d %s" % (fid, size, claimed, p))
    if show_paths and lz11_ids:
        print("  all LZ11 ids:", ", ".join(str(i) for i, _, _ in lz11_ids))
    print()

    print("== extension x magic (top extensions) ==")
    print("  %-10s %-6s %s" % ("ext", "files", "signatures"))
    for ext, c in sorted(exts.items(), key=lambda x: -sum(x[1].values()))[:18]:
        top = ", ".join("%s:%d" % (k, v) for k, v in c.most_common(4))
        print("  %-10s %-6d %s" % (ext, sum(c.values()), top))
    print()
    print("== one example per class ==")
    for c, lst in sorted(examples.items()):
        fid, p, size, head = lst[0]
        print("  %-10s id=%-6d size=%-9d %s" % (c, fid, size, p))
        print("               %s" % head[:16].hex(" "))


if __name__ == "__main__":
    main()
