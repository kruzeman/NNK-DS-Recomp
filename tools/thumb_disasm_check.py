#!/usr/bin/env python3
"""Phase-3A3 Thumb disassembly cross-check — nnktool vs binutils vs llvm-mc.

Three sources per sampled instruction:
  1. nnktool disasm --mode thumb   (the recompiler's own decoder)
  2. arm-none-eabi-objdump -M force-thumb  (binutils)
  3. llvm-mc --triple thumbv5te    (LLVM): every nnktool line is RE-ASSEMBLED
     and must reproduce the exact encoding byte-for-byte.

Sections
  A. encoding battery — at least one representative per ARMv5TE Thumb format
     (formats 1-18 + BL/BLX halfword pairs + suffix-validity cases), compared
     STRICTLY against both references (every slot must be present in both).
  B. real Thumb function bodies — every Thumb instruction the recovery
     produced (work/phase3a/arm9_decompile.txt), compared at its own address
     against binutils.  Ranges are the recovered bodies, i.e. genuine ARMv5TE
     16-bit code; binutils' Thumb-2 decoding of surrounding data is excluded
     on both sides by construction.
  C. interworking unit check — the generated PC-dispatch image must carry the
     ARM call site, the Thumb callee case, and the link/dispatch stores that
     connect them (ARM blx -> Thumb, Thumb bx lr -> ARM).

Usage:
  python3 tools/thumb_disasm_check.py [--listing work/phase3a/arm9_decompile.txt]
                                      [--image work/ghidra_src/arm9.bin]
                                      [--c work/phase3a/arm9_thumb.c]
Exit code 0 only when every check passes.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cmp_objdump import LINE, norm  # noqa: E402

NNK = os.path.join(HERE, "..", "nnktool", "target", "release", "nnktool")
BASE = 0x02000000

# ---------------------------------------------------------------- section A --
# (label, halfword) — one representative per Thumb format, plus suffix pairs.
BATTERY = [
    ("1 lsls imm", 0x0088), ("1 lsls #0 (movs)", 0x0000), ("1 lsrs #0", 0x0808),
    ("1 lsrs imm", 0x0888), ("1 asrs #0", 0x1008), ("1 asrs imm", 0x1088),
    ("2 add reg", 0x1808), ("2 sub reg", 0x1A48), ("2 add imm3", 0x1C08),
    ("2 sub imm3", 0x1E08), ("2 sub imm3 max", 0x1FC7),
    ("3 movs", 0x2005), ("3 cmp", 0x2805), ("3 adds", 0x3005), ("3 subs", 0x3805),
    ("4 ands", 0x4009), ("4 eors", 0x4049), ("4 lsls reg", 0x4081),
    ("4 lsrs reg", 0x40C1), ("4 asrs reg", 0x4101), ("4 adcs", 0x4141),
    ("4 sbcs", 0x4181), ("4 rors", 0x41C1), ("4 tst", 0x4201),
    ("4 negs", 0x4241), ("4 cmp", 0x4281), ("4 cmn", 0x42C1),
    ("4 orrs", 0x4301), ("4 muls", 0x4341), ("4 bics", 0x4381), ("4 mvns", 0x43C1),
    ("4 muls rd!=rm", 0x4350),
    ("5 add hi", 0x44F0), ("5 cmp hi", 0x45E8), ("5 mov hi", 0x4680),
    ("5 mov hi r15", 0x4687), ("5 add hi pc", 0x4478), ("5 bx lr", 0x4770),
    ("5 blx r0", 0x4780), ("5 bx r0", 0x4700), ("5 nop", 0x46C0),
    ("6 ldr lit", 0x4805), ("6 ldr lit rd7", 0x4FFF),
    ("7 str", 0x5049), ("7 strb", 0x5449), ("7 ldr", 0x5849), ("7 ldrb", 0x5C49),
    ("8 HS=10 ldrh", 0x5A49), ("8 HS=01 ldrsb", 0x5649), ("8 HS=11 ldrsh", 0x5E49),
    ("8 HS=00 strh", 0x5249),
    ("9 str", 0x6041), ("9 ldr", 0x6841), ("9 strb", 0x7041), ("9 ldrb", 0x7841),
    ("10 strh", 0x8041), ("10 ldrh", 0x8841),
    ("11 str sp", 0x9041), ("11 ldr sp", 0x9841),
    ("12 add pc", 0xA110), ("12 add sp", 0xA910),
    ("13 add sp", 0xB000), ("13 sub sp", 0xB084),
    ("14 push", 0xB401), ("14 push lr", 0xB501), ("14 pop", 0xBC01),
    ("14 pop pc", 0xBD01),
    ("15 stmia", 0xC041), ("15 ldmia base in list", 0xC841),
    ("15 ldmia base free", 0xC842), ("15 stmia base free", 0xC141),
    ("16 beq", 0xD001), ("16 bge", 0xDA01), ("16 ble", 0xDD01),
    ("17 udf", 0xDE10), ("17 svc", 0xDF10), ("17 svc 0b", 0xDF0B),
    ("18 b fwd", 0xE001), ("18 b back", 0xE7FF),
]
# BL/BLX pairs: (first, second, valid?) — valid iff binutils decodes them as
# a branch (a BLX suffix must have bit0 clear, see decode.rs).
PAIRS = [(0xF000, 0xF800, True), (0xF7FF, 0xFFFF, True), (0xF000, 0xE000, True),
         (0xF000, 0xE7FE, True), (0xF4F7, 0xF000, True), (0xF000, 0xE7FF, False),
         (0xF000, 0xF001, True)]

BRANCH_RE = re.compile(r"^(b|bl|blx|beq|bne|bcs|bcc|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble)"
                       r"(\.n|\.w)?$")


def run(cmd, stdin=None):
    return subprocess.run(cmd, input=stdin, capture_output=True, text=True)


def nnk_disasm(path, start, end, mode="thumb"):
    r = run([NNK, "disasm", path, "--base", hex(BASE), "--start", hex(start),
             "--end", hex(end), "--mode", mode])
    out = {}
    for line in r.stdout.splitlines():
        m = LINE.match(line.rstrip("\n"))
        if m:
            out[int(m.group(1), 16)] = (m.group(2).lower(), norm(m.group(3)))
    return out


def objdump_disasm(path, start, end, vma=None):
    # For a SLICE of the image the file starts at `start`, so the vma offset
    # must map file offset 0 -> start (a whole-image/battery file starts at
    # BASE).  Using BASE for a slice would shift every address and make the
    # comparison look like "not decoded by binutils".
    if vma is None:
        vma = start
    r = run(["arm-none-eabi-objdump", "-D", "-b", "binary", "-m", "arm",
             "-M", "force-thumb", "--adjust-vma=" + hex(vma),
             "--start-address=" + hex(start), "--stop-address=" + hex(end), path])
    out = {}
    for line in r.stdout.splitlines():
        m = LINE.match(line.rstrip("\n"))
        if m:
            out[int(m.group(1), 16)] = (m.group(2).lower(), norm(m.group(3)))
    return out


def llvm_roundtrip(addr, text, raw_bytes, failures):
    """Re-assemble nnktool's text with llvm-mc; the bytes must match.

    Returns True (match), False (mismatch/rejected) or None (no mnemonic to
    round-trip: an invalid BL/BLX suffix pair)."""
    t = " ".join(text.split())
    if not t:
        return None
    # A BL/BLX pair parses as "<second halfword> <mnemonic> <target>": the
    # word column of both tools only holds the first halfword.  Drop the
    # suffix token (and treat a pair with no mnemonic — an invalid suffix —
    # as skipped: binutils decodes those as an unrelated instruction, so
    # there is no shared text to compare).
    parts0 = t.split(None, 1)
    if re.fullmatch(r"[0-9a-f]{4}", parts0[0]):
        if len(parts0) == 1:
            return None
        t = parts0[1].strip()
        if not t:
            return None
    parts = t.split(None, 1)
    mn = parts[0]
    ops = parts[1] if len(parts) > 1 else ""
    if BRANCH_RE.match(mn) and re.search(r"0x[0-9a-fA-F]+", ops):
        # Branches: llvm-mc's ASSEMBLER wants the raw encoded offset (and for
        # BLX a 4-byte-aligned one), while nnktool prints the absolute,
        # already-aligned target.  Cross-check the two by DISASSEMBLING the
        # bytes with llvm-mc and comparing its (mnemonic, offset) against
        # nnktool's target: target == (addr + 4 + llvm_off) with the BLX
        # 4-byte alignment applied — exactly what decode_thumb does.
        r = run(["llvm-mc", "-disassemble", "--triple=thumbv5te-unknown-unknown"],
                stdin=" ".join("0x%02x" % b for b in raw_bytes) + "\n")
        m = re.search(r"\b(b|bl|blx|b\w\w)\s+#(-?\d+)", r.stdout)
        if not m:
            failures.append((addr, text, "llvm-mc disasm gave no branch: %r"
                             % r.stdout.strip()[:60]))
            return False
        lm = m.group(1)
        loff = int(m.group(2))
        expected = (addr + 4 + loff) & (0xFFFFFFFC if lm == "blx" else 0xFFFFFFFF)
        want = re.search(r"0x[0-9a-fA-F]+", ops)
        if not want:
            failures.append((addr, text, "no target in nnktool text"))
            return False
        got = int(want.group(0), 16)
        if lm != mn.split(".")[0] or got != expected:
            failures.append((addr, text, "llvm %s off=%d -> 0x%08x, nnk 0x%08x"
                             % (lm, loff, expected, got)))
            return False
        return True
    elif mn == "add" and re.match(r"r\d+,\s*pc,", ops):
        # binutils prints `add rD, pc, #imm`, llvm prints/assembles `adr rD,
        # #imm` — same encoding (Thumb format 12), different spelling.
        mn = "adr"
        ops = re.sub(r"^r\d+,\s*pc,\s*", lambda m: m.group(0).split(",")[0] + ", ", ops)
    src = ".text\n%s %s\n" % (mn, ops)
    r = run(["llvm-mc", "-assemble", "--triple=thumbv5te-unknown-unknown",
             "--show-encoding"], stdin=src)
    m = re.search(r"encoding:\s*\[([^\]]*)\]", r.stdout)
    if not m:
        failures.append((addr, text, "llvm-mc rejected: %s / %s"
                         % (r.stdout.strip()[:60], r.stderr.strip()[:60])))
        return False
    got = bytes(int(x, 16) for x in m.group(1).split(","))
    if got != raw_bytes:
        failures.append((addr, text, "llvm-mc assembled %s, want %s"
                         % (got.hex(), raw_bytes.hex())))
        return False
    return True


def section_a(image, tmpdir):
    path = os.path.join(tmpdir, "thumb_battery.bin")
    slots = []
    data = b""
    for label, h in BATTERY:
        slots.append((BASE + len(data), label, struct.pack("<H", h), True))
        data += struct.pack("<H", h)
    for h1, h2, valid in PAIRS:
        slots.append((BASE + len(data), "pair %04x %04x" % (h1, h2),
                      struct.pack("<HH", h1, h2), valid))
        data += struct.pack("<HH", h1, h2)
    with open(path, "wb") as fh:
        fh.write(data)
    end = BASE + len(data)
    mine = nnk_disasm(path, BASE, end)
    theirs = objdump_disasm(path, BASE, end, vma=BASE)
    # Invalid BL/BLX suffix pairs: binutils decodes the prefix as an
    # unrelated instruction (`bfcsel`), so there is no shared text — those
    # slots still count for the ENCODING check (both sides carry the bytes).
    skip_text = {a for a, label, raw, valid in slots if not valid}
    addrs = sorted(set(mine) | set(theirs))
    enc_ok = txt_ok = 0
    txt_n = 0
    bad = []
    for a in addrs:
        if a not in mine or a not in theirs:
            bad.append("%08x missing on one side (nnk=%s objdump=%s)"
                       % (a, a in mine, a in theirs))
            continue
        if mine[a][0] == theirs[a][0]:
            enc_ok += 1
        else:
            bad.append("%08x encoding nnk=%s objdump=%s" % (a, mine[a][0], theirs[a][0]))
        if a in skip_text:
            continue
        txt_n += 1
        if mine[a][1] == theirs[a][1]:
            txt_ok += 1
        else:
            bad.append("%08x text nnk=%r objdump=%r" % (a, mine[a][1], theirs[a][1]))
    # llvm-mc round trip on every slot's nnk line
    rt_fail = []
    rt_ok = 0
    rt_skip = 0
    for a, label, raw, valid in slots:
        if a in mine and mine[a][1]:
            res = llvm_roundtrip(a, mine[a][1], raw, rt_fail)
            if res is None:
                rt_skip += 1
            elif res:
                rt_ok += 1
    print("A. encoding battery (%d slots: %d formats + %d BL/BLX pairs)"
          % (len(slots), len(BATTERY), len(PAIRS)))
    print("   vs binutils     : %d/%d encoding, %d/%d text (%d invalid-suffix slots text-excluded)"
          % (enc_ok, len(addrs), txt_ok, txt_n, len(skip_text)))
    print("   vs llvm-mc      : %d/%d round-trip (text -> bytes, %d skipped: invalid suffix)"
          % (rt_ok, len(slots) - rt_skip, rt_skip))
    for b in bad[:10]:
        print("   MISMATCH " + b)
    for f in rt_fail[:10]:
        print("   LLVM    %08x %s -> %s" % f)
    return not bad and not rt_fail


def parse_thumb_bodies(listing):
    """[(start, end, [(addr, text)])] for every recovered Thumb function."""
    bodies = []
    cur = None
    hdr = re.compile(r"^== sub_([0-9A-Fa-f]+)_thumb .*==\s*$")
    line_re = re.compile(r"^\s{2}([0-9a-f]{8}):\s+(\S+)\s+(.*)$")
    with open(listing) as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = hdr.match(line)
            if m:
                if cur:
                    bodies.append(cur)
                cur = {"start": int(m.group(1), 16), "lines": []}
                continue
            if cur is not None:
                if line.startswith("== "):
                    bodies.append(cur)
                    cur = None
                    continue
                m = line_re.match(line)
                if m and cur is not None:
                    cur["lines"].append((int(m.group(1), 16), m.group(3).strip()))
    if cur:
        bodies.append(cur)
    return bodies


def section_b(image, listing, tmpdir):
    if not os.path.exists(listing):
        print("B. recovered Thumb bodies : listing %s missing" % listing)
        return False
    bodies = parse_thumb_bodies(listing)
    if not bodies:
        print("B. recovered Thumb bodies : none found in %s" % listing)
        return False
    total = ok = 0
    bad = []
    for b in bodies:
        addrs = [a for a, _ in b["lines"]]
        start = min(addrs)
        end = max(addrs) + 4
        binpath = os.path.join(tmpdir, "b_%08x.bin" % start)
        with open(binpath, "wb") as fh:
            off = start - BASE
            fh.write(image[off:off + (end - start)])
        theirs = objdump_disasm(binpath, start, end)
        for a, text in b["lines"]:
            total += 1
            if a not in theirs:
                bad.append("%08x not decoded by binutils" % a)
                continue
            if norm(text) == theirs[a][1]:
                ok += 1
            else:
                bad.append("%08x nnk=%r binutils=%r" % (a, norm(text), theirs[a][1]))
    print("B. recovered Thumb bodies : %d functions, %d/%d instructions match binutils"
          % (len(bodies), ok, total))
    for x in bad[:10]:
        print("   MISMATCH " + x)
    return not bad and total > 0


def section_c(c_path):
    """Interworking unit check on the generated dispatch image."""
    if not os.path.exists(c_path):
        print("C. interworking          : %s missing" % c_path)
        return False
    with open(c_path) as fh:
        src = fh.read()
    # observed pair: ARM `blx` at 0x0201C178 -> Thumb 0x020004A8, which
    # returns via `bx lr` to 0x0201C17C (melonDS ground truth, index 3633732).
    checks = [
        ("ARM call site case", "case 0x0201C178u:"),
        ("ARM call dispatches to Thumb callee", "pc = 0x020004A8u;"),
        ("link set with Thumb bit", "r[14] = 0x0201C17Du;"),
        ("Thumb callee case", "case 0x020004A8u:"),
        ("Thumb svc #0x0B (CpuSet) site", "hle_swi(0x00000Bu, 0x020004A8u, 1)"),
        ("Thumb return masks the state bit", "pc = _t & ~1u;"),
        ("second Thumb thunk case", "case 0x02000648u:"),
        ("Thumb WaitByLoop site", "hle_swi(0x000003u, 0x02000648u, 1)"),
    ]
    bad = []
    for name, needle in checks:
        if needle not in src:
            bad.append("%s: %r not found" % (name, needle))
    print("C. interworking (generated C) : %d/%d checks" % (len(checks) - len(bad), len(checks)))
    for x in bad:
        print("   MISSING " + x)
    return not bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="work/ghidra_src/arm9.bin")
    ap.add_argument("--listing", default="work/phase3a/arm9_decompile.txt")
    ap.add_argument("--c", dest="cfile", default="work/phase3a/arm9_thumb.c")
    args = ap.parse_args()
    image = open(args.image, "rb").read()
    ok = True
    with tempfile.TemporaryDirectory(prefix="nnkthumb_", dir=os.environ.get("TMPDIR", "/tmp")) as td:
        a = section_a(image, td)
        b = section_b(image, args.listing, td)
        c = section_c(args.cfile)
        ok = a and b and c
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
