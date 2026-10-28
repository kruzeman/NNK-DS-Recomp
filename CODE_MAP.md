# CODE_MAP.md — Ni no Kuni DS (B2KJ) ARM9: recovered code map (Phase-1A)

Scope: ARM9 boot recovery from the decompressed image with `nnktool`
(`disasm` / `decompile` / `recompile-entry`).  Everything here is generated
from `work/ghidra_src/arm9.bin` (0x6CB78 bytes, load @ `0x02000000`, entry
`0x02000800`, ARM state at entry).  The stored `work/arm9.bin` is *not*
analyzed — code beyond 0x4000 is still compressed in it.

Reproduce every number below with the commands in
[Reproducing](#reproducing) at the end of this file.

## Boot path recovered from the entry

The entry function `0x02000800` is the ARM9 startup stub: it enables the
display power domain, sets up the SVC/IRQ/SYS stacks, clears three RAM
regions through `sub_0200093C` (word-fill loop), flushes caches with three
`mcr p15` writes per line, then tail-jumps (`bx r1` @ `0x02000914`) into the
C runtime start at `0x02000C8C`.

Recovered boot-path functions (all ARM state):

| entry | role | insns | blocks |
|---|---|---|---|
| `0x02000800` | ARM9 startup stub (entry) | 70 | 4 |
| `0x02000A78` | CP15 setup (MPU/caches, MCR p15 sequence) | 46 | 1 |
| `0x0200093C` | word-fill helper (called 3x) | 5 | 2 |
| `0x02000950` | ARM9 decompressor/continuation (tail-branches to `0x02000218`) | 53 | 10 |
| `0x020009FC` | init-array walker | 30 | 7 |

Unresolved control-flow sites in the entry: `0x02000914` `bx r1` (r1 is
loaded from the literal pool two instructions earlier — a veneer my walker
does not resolve backwards), plus 25 further indirect sites across the
recovered graph.

## Coverage (honest numbers)

* Recovery root `0x02000800` (ARM): **43 functions, 43 ARM / 0 Thumb**,
  2 228 instruction bytes = **0.50 %** of the 0x6CB78-byte image,
  0 terminations on undefined instructions, 26 unresolved indirect sites.
* Recovery root `0x02060C00` (Thumb, `--entry-mode thumb`): **3 functions
  (2 Thumb, 1 ARM)**, 540 bytes.  The ARM function `0x020466F4` is reached
  through the Thumb `blx 0x020466f4` at `0x02060C2E`, i.e. this is a real
  ARM↔Thumb interworking transition, not a heuristic.

Coverage is deliberately reachability-limited: only direct calls and
statically resolvable veneers are followed.  Jump tables, function-pointer
dispatch and `ldr pc, [pc, rN, lsl #2]` switches are recorded as unresolved
indirect sites, not expanded — that is why the reachable set is small
against Ghidra's full-image analysis.

## Cross-check against Ghidra (`work/ghidra/nnk.gpr`, program `/arm9.bin`)

Ghidra 12.1.4, `ARM:LE:32:v5`, base `0x02000000`, auto-analysis completed;
function table exported by `work/ghidra_scripts/DumpFunctions.java`.

| metric | value |
|---|---|
| Ghidra functions (whole image) | 1 942 |
| Ghidra Thumb-classified entries (2-byte entry insn) | 0 |
| nnktool functions recovered from entry | 43 |
| agreed (Ghidra function at the same entry address) | 19 / 43 (44.2 %) |
| mode agreement among those | 19 / 19 |
| nnktool-only entries | 24 (2 lie inside a Ghidra body, 22 have no Ghidra function) |
| Ghidra-only functions inside nnktool's span `0x02000800..0x020463A0` | 1 406 |

Honest reading: the two analyses disagree substantially, in both
directions.  Ghidra's heuristics never created a function at the ARM9 entry
point `0x02000800` itself and classified no Thumb code at all, while
nnktool recovers the entry and the Thumb function at `0x02060C00` cleanly.
Neither set is a superset of the other; nnktool's set is the direct-call
reachable closure, Ghidra's is a full-image heuristic sweep.

## `recompile-entry` (first native recompile)

`nnktool recompile-entry` translates **the entry function only** into host C:

* one C function with a PC-dispatch `switch` loop over the recovered CFG
  (70 instruction cases, 4 blocks);
* guest state: `r[16]`, NZCV flags, CPSR control byte (single-bank model —
  `msr cpsr_c` writes the control byte but there are no banked registers);
* flat guest memory: one 16 MiB window at `0x02000000`; reads outside it
  return 0, writes are dropped (documented simplification);
* `mcr`/`mrc` p15: writes are no-ops, reads return 0;
* the 9 direct calls are emitted as **logged call shims** — every shim prints
  `call-shim 0xTARGET from 0xSOURCE (callee not executed in Phase-1A)` on
  stderr, so an unexecuted callee can never be mistaken for a successful one;
* the guest runs 10 517 instructions and halts at the tail `bx r1`
  (dispatch to untranslated `0x02000C8C`).

Guest-PC trace modes (both print one `0x%08x` per line):

* **default — first-visit trace**: prints each guest PC the first time the
  dispatch loop reaches it.  The entry visits 70 distinct PCs; the first 64
  are exactly the 64 addresses in `work/entry_disasm_check.txt` (**64/64
  match**).
* **`-DNNK_TRACE_EXEC` — per-execution trace**: prints every executed
  instruction, so loops repeat (e.g. the cache-clean loop at
  `0x020008CC..0x020008E0` runs 1 744 iterations).  The golden file is a
  linear listing, hence the 64-entry check is defined on the first-visit
  trace; both modes are verified to run.

## Disassembler accuracy

`nnktool disasm` was diffed against `arm-none-eabi-objdump` on four ranges
(445 instructions total, whitespace/comment/number-format normalised):

| range | mode | instructions | encoding match | text match |
|---|---|---|---|---|
| `0x02000800..0x02000B00` | arm | 192 | 192/192 | 192/192 |
| `0x0200B904..0x0200BA04` | arm | 64 | 64/64 | 64/64 |
| `0x0200C8C0..0x0200C9C0` | arm | 64 | 64/64 | 64/64 |
| `0x02060C00..0x02060D00` | thumb | 125 | 125/125 | 125/125 |

## Known limitations (Phase-1A)

1. `recompile-entry` translates the entry function only; callees are shimmed.
2. Single-bank register model: `msr cpsr_c` mode switches do not bank r8-r14.
3. Flat memory window: no MMU/I/O semantics (reads of I/O return 0, which is
   what lets the entry's POWCNT1 poll at `0x02000808` fall through).
4. Veneer resolution looks one instruction backwards (`add rM, pc, #imm` /
   `ldr rM, =` immediately before `bx rM`); pool-fed tail jumps such as the
   entry's `bx r1` remain unresolved.
5. Thumb is decoded and recovered, but not yet translated to C.
6. Literal pools are only excluded from code by control flow: a
   fall-through into a pool would mis-decode (none observed in the
   recovered sets — 0 undefined-instruction terminations).

## Reproducing

All commands run from the repository root; outputs land in `work/`
(never committed).

```sh
# build
cargo build --release --manifest-path nnktool/Cargo.toml

# 1) disassembler vs arm-none-eabi-objdump (4 sampled ranges)
nnktool/target/release/nnktool disasm work/ghidra_src/arm9.bin \
  --base 0x02000000 --start 0x02000800 --end 0x02000B00 --mode arm > work/nnk_d_entry.txt
arm-none-eabi-objdump -D -b binary -m arm --adjust-vma=0x02000000 \
  --start-address=0x02000800 --stop-address=0x02000B00 work/ghidra_src/arm9.bin > work/obj_entry.txt
python3 tools/cmp_objdump.py work/nnk_d_entry.txt work/obj_entry.txt

# 2) function recovery from the entry (+ Thumb-rooted run)
nnktool/target/release/nnktool decompile work/ghidra_src/arm9.bin work/phase1a \
  --base 0x02000000 --entry 0x02000800
nnktool/target/release/nnktool decompile work/ghidra_src/arm9.bin work/phase1a/thumb \
  --base 0x02000000 --entry 0x02060C00 --entry-mode thumb

# 3) Ghidra cross-check (Ghidra project already analysed)
/home/hermes/tools/ghidra/support/analyzeHeadless work/ghidra nnk -process arm9.bin \
  -noanalysis -postScript DumpFunctions.java work/ghidra_arm9_functions.tsv \
  -scriptPath tools/ghidra
python3 tools/crosscheck_ghidra.py work/phase1a/arm9_functions.tsv work/ghidra_arm9_functions.tsv

# 4) recompile entry -> C -> cc -> run -> first-64 guest-PC trace check
nnktool/target/release/nnktool recompile-entry work/ghidra_src/arm9.bin \
  work/phase1a/arm9_entry.c --base 0x02000000 --entry 0x02000800
cc -O2 -o work/phase1a/arm9_entry_recompiled work/phase1a/arm9_entry.c
work/phase1a/arm9_entry_recompiled work/phase1a/arm9_entry.image.bin \
  > work/phase1a/entry_trace.txt 2> work/phase1a/entry_stderr.txt
python3 tools/check_entry_trace.py work/phase1a/entry_trace.txt work/entry_disasm_check.txt 64
```
