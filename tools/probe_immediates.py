#!/usr/bin/env python3
"""Probe how arm-none-eabi-objdump renders ARM rotated immediates.

Emits `mov rN, #value` for a matrix of values, assembles, disassembles and
reports which ones objdump prints in the raw `#imm8, rot` form.
"""
import re
import subprocess

vals = []
for imm8 in (0x01, 0x03, 0x0F, 0x13, 0x1F, 0x3F, 0x7F, 0xFF, 0x22, 0x88, 0x9C, 0xFA, 0xC3):
    for rot in range(0, 32, 2):
        v = ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8
        vals.append((v, imm8, rot))

lines = ["\t.text", "\t.arm"]
for i, (v, imm8, rot) in enumerate(vals):
    if v >= 0x80000000:
        dec = str(v - (1 << 32))
    else:
        dec = str(v)
    lines.append("\tmov r%d, #%s" % (i % 14, dec))
open("/tmp/imm.s", "w").write("\n".join(lines) + "\n")

r = subprocess.run(["arm-none-eabi-as", "-mcpu=arm946e-s", "-o", "/tmp/imm.o", "/tmp/imm.s"],
                   capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[:2000])
    raise SystemExit(1)
r = subprocess.run(["arm-none-eabi-objdump", "-d", "/tmp/imm.o"], capture_output=True, text=True)

ROT = re.compile(r"#(0x[0-9a-f]+), (0x[0-9a-f]+)")
DEC = re.compile(r"#(-?\d+)\s*(?:@ (0x[0-9a-f]+))?")
WORD = re.compile(r"^\s+[0-9a-f]+:\t([0-9a-f]{8}) \tmov\tr\d+, (.*)$")

rows = []
for line in r.stdout.splitlines():
    m = WORD.match(line)
    if not m:
        continue
    word = int(m.group(1), 16)
    text = m.group(2).strip()
    imm8 = word & 0xFF
    rot = ((word >> 8) & 0xF) * 2
    v = (((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF) if rot else imm8
    rows.append((v, imm8, rot, text))

rotated = [(v, i8, rt, t) for (v, i8, rt, t) in rows if "," in t]
decimal = [(v, i8, rt, t) for (v, i8, rt, t) in rows if "," not in t]
print("total %d, rotated-form %d, decimal-form %d" % (len(rows), len(rotated), len(decimal)))
print("\nrotated-form examples (value, imm8, rot, objdump text):")
for v, i8, rt, t in rotated[:25]:
    print("  0x%08X  imm8=0x%02x rot=%2d  %s" % (v, i8, rt, t))
print("\ndecimal-form with rot>0 (value, imm8, rot):")
shown = 0
for v, i8, rt, t in decimal:
    if rt > 0:
        print("  0x%08X  imm8=0x%02x rot=%2d  %s" % (v, i8, rt, t))
        shown += 1
        if shown > 25:
            break
