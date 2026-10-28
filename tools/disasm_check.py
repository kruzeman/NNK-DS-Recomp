#!/usr/bin/env python3
"""
tools/disasm_check.py — three-way cross-check of the ARM9 entry disassembly.

  1. capstone      (direct decode of the ROM bytes)
  2. arm-none-eabi-objdump (independent binutils decode)
  3. llvm-mc       (re-assemble the capstone text, emit an object, disassemble
                    it with llvm-objdump, compare encodings against the ROM)

Usage:  /usr/bin/python3 tools/disasm_check.py [rom] [n_insns]
Default: "Ni No Kuni.nds", 64 instructions.

Also reports the CPU state at entry (ARM vs Thumb) and any interwork /
CPSR-mode switches inside the window.
"""
import re
import struct
import subprocess
import sys
import tempfile
import os

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

ROM_DEFAULT = "Ni No Kuni.nds"
N_DEFAULT = 64

BR_RE = re.compile(r"^(blx|bl|b)(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al|nv)?$")
HEX_RE = re.compile(r"^#(0x[0-9a-fA-F]+|-?0x[0-9a-fA-F]+)$")
ODUMP_RE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{8})\s*(.*)$")


def read_arm9(rom_path):
    d = open(rom_path, "rb").read(0x400)
    arm9_rom = struct.unpack_from("<I", d, 0x20)[0]
    arm9_entry = struct.unpack_from("<I", d, 0x24)[0]
    arm9_ram = struct.unpack_from("<I", d, 0x28)[0]
    arm9_size = struct.unpack_from("<I", d, 0x2C)[0]
    blob = open(rom_path, "rb").read()[arm9_rom:arm9_rom + arm9_size]
    return blob, arm9_entry, arm9_ram, arm9_size


def capstone_disasm(blob, entry, ram, n):
    off = entry - ram
    code = blob[off:off + n * 4 + 4]
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    out = []
    for ins in md.disasm(code, entry):
        out.append((ins.address, code[ins.address - entry:ins.address - entry + ins.size],
                    ins.mnemonic, ins.op_str, ins.size))
        if len(out) == n:
            break
    return out


def thumb_probe(blob, entry, ram, n=4):
    off = entry - ram
    code = blob[off:off + n * 4]
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    return [(i.address, i.mnemonic, i.op_str) for i in md.disasm(code, entry)]


def objdump_words(blob, entry, ram, n):
    off = entry - ram
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob[off:off + n * 4])
        path = f.name
    try:
        r = subprocess.run(
            ["arm-none-eabi-objdump", "-D", "-b", "binary", "-m", "arm",
             "--start-address=0", "--stop-address=0x%x" % (n * 4), path],
            capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("objdump failed: " + r.stderr)
        words, text = [], []
        for line in r.stdout.splitlines():
            m = ODUMP_RE.match(line)
            if m:
                # temp file starts at the entry -> parsed address is the
                # offset within the disassembly window
                words.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
                text.append(line.rstrip())
        return words, text
    finally:
        os.unlink(path)


def llvm_roundtrip(insns):
    """Assemble capstone text with llvm-mc -> object -> llvm-objdump words."""
    lines = ["\t.arch\tarmv5te", "\t.text"]
    for addr, _b, mn, op, _s in insns:
        if BR_RE.match(mn) and HEX_RE.match(op):
            target = int(op[1:], 16)
            delta = target - addr
            expr = ". + 0x%x" % delta if delta >= 0 else ". - 0x%x" % -delta
            lines.append("\t%s\t%s" % (mn, expr))
        else:
            lines.append("\t%s\t%s" % (mn, op))
    src = "\n".join(lines) + "\n"
    with tempfile.TemporaryDirectory() as td:
        s = os.path.join(td, "e.s")
        o = os.path.join(td, "e.o")
        open(s, "w").write(src)
        r = subprocess.run(["llvm-mc", "-arch=arm", "-triple=armv5te-unknown-unknown",
                            "-filetype=obj", s, "-o", o], capture_output=True, text=True)
        if r.returncode != 0:
            return None, src, "llvm-mc failed:\n" + r.stderr
        r = subprocess.run(["llvm-objdump", "-d", o], capture_output=True, text=True)
        if r.returncode != 0:
            return None, src, "llvm-objdump failed:\n" + r.stderr
        words, text = [], []
        for line in r.stdout.splitlines():
            m = ODUMP_RE.match(line)
            if m:
                words.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
                text.append(line.rstrip())
        return words, src, ""


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else ROM_DEFAULT
    n = int(sys.argv[2]) if len(sys.argv) > 2 else N_DEFAULT

    blob, entry, ram, size = read_arm9(rom)
    print("ROM: %s" % rom)
    print("ARM9: %d bytes @ 0x%08X, entry 0x%08X (image offset 0x%X)" % (size, ram, entry, entry - ram))
    print("entry low bit = %d -> %s state at entry"
          % (entry & 1, "THUMB" if entry & 1 else "ARM"))
    print()

    cs = capstone_disasm(blob, entry, ram, n)
    print("[1] capstone: decoded %d/%d instructions (all size 4: %s)"
          % (len(cs), n, all(i[4] == 4 for i in cs)))
    if len(cs) != n:
        print("FAIL: capstone did not produce %d instructions" % n)
        sys.exit(1)

    od, od_text = objdump_words(blob, entry, ram, n)
    print("[2] arm-none-eabi-objdump: %d instructions" % len(od))

    lw, src, err = llvm_roundtrip(cs)
    if lw is None:
        print("[3] llvm-mc: %s" % err)
        sys.exit(1)
    print("[3] llvm-mc re-assembly -> object -> llvm-objdump: %d instructions" % len(lw))

    ok = True
    rows = []
    for i, ((addr, b, mn, op, _s), o, l) in enumerate(zip(cs, od, lw)):
        orig = struct.unpack("<I", bytes(b))[0]
        o_addr, o_word, o_txt = o
        l_addr, l_word, l_txt = l
        # compare in WINDOW-offset space (0-based from the entry):
        #   capstone uses absolute addresses, objdump/llvm are window-relative.
        win = addr - entry
        match = (orig == o_word == l_word)
        addr_ok = (o_addr == win) and (l_addr == win)
        st = "OK " if (match and addr_ok) else "BAD"
        if not (match and addr_ok):
            ok = False
        rows.append((i, addr, orig, o_word, l_word, mn, op, o_txt, l_txt, st))

    print()
    print("%-4s %-10s %-10s %-10s %-10s %-6s %s" % ("#", "addr", "rom", "objdump", "llvm-mc", "state", "instruction"))
    for (i, addr, orig, o_word, l_word, mn, op, o_txt, l_txt, st) in rows:
        print("%-4d 0x%08X 0x%08X 0x%08X 0x%08X %-6s %s %s%s"
              % (i, addr, orig, o_word, l_word, st, mn, op, "" if st == "OK " else "   <<< MISMATCH"))

    # Thumb sanity: decode the same bytes as if entry were Thumb (informational)
    th = thumb_probe(blob, entry, ram, 4)
    print()
    print("Thumb-mode decode of the same entry bytes (proves ARM is the correct state):")
    for a, mn, op in th:
        print("    0x%08X: %s %s" % (a, mn, op))
    arm_first = cs[0]
    print("    ARM first insn: 0x%08X: %s %s" % (arm_first[0], arm_first[2], arm_first[3]))

    # mode / interwork switches in the window
    print()
    print("Interwork / CPSR-mode instructions in the window:")
    found = False
    for (addr, b, mn, op, _s) in cs:
        if mn in ("bx", "blx") or mn.startswith("msr"):
            found = True
            print("    0x%08X: %s %s" % (addr, mn, op))
    if not found:
        print("    (none)")

    print()
    print("RESULT: %s — %s" % ("PASS" if ok else "FAIL",
          "capstone, arm-none-eabi-objdump and llvm-mc agree on all %d instructions" % n
          if ok else "mismatch detected above"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
