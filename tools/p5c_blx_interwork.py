#!/usr/bin/env python3
"""Phase-5C: apply the Thumb-BLX-immediate interworking state fix to
tools/nnk_smc.inc (idempotent)."""
p = '/home/hermes/NiNoKuni/worktrees/stack-div/tools/nnk_smc.inc'
s = open(p).read()
if 'smc_branch_mode' in s:
    raise SystemExit('already applied')

decl = """static int smc_t_sticky;                  /* Phase-4Z: last resolved T bit */
/* Phase-5C: the processor state CARRIED by an interworking branch.  The T bit
 * is part of the ARM9 state, not of the PC: a Thumb `blx <imm>` branches to an
 * ARM-state target (ARM ARM A7.1.4: BLX always switches state), while `bl`
 * stays in Thumb.  The live re-decoder had only the compiled map and a sticky
 * last-state guess, so the target was re-decoded in the WRONG state: measured
 * at 0x020D2418 `blx 0x020E7968` (Thumb word 0xF015 0xEAA6) - the target holds
 * the ARM `push {r3,r4,r5,lr}` (0xE92D4038), the run re-decoded it as Thumb
 * (`stmdb sp!,{ip,lr}`), walked the pointer table at 0x02160394 for 25,284
 * PCs through the 0x0214xxxx-0x0217xxxx band and halted at 0x02178E9E on the
 * data word 0x1C36F000 (a "coprocessor" encoding = data executed as code).
 * The state is recorded at the branch and honoured for exactly the next
 * dispatch (the step stamp keeps a loop revisit from inheriting it). */
static uint32_t smc_t_pc = 0xFFFFFFFFu;
static int smc_t_val = -1;
static unsigned long long smc_t_step;
static void smc_branch_mode(uint32_t raw_target) {
    smc_t_pc = raw_target & ~1u;
    smc_t_val = (int)(raw_target & 1u);
    smc_t_step = steps;
}
static int smc_carried_thumb(uint32_t pc, int *state) {
    if (smc_t_val < 0 || smc_t_pc != pc || steps != smc_t_step + 1ull) return 0;
    *state = smc_t_val;
    return 1;
}
"""
old = "static int smc_t_sticky;                  /* Phase-4Z: last resolved T bit */\n"
assert s.count(old) == 1
s = s.replace(old, decl, 1)

a = """            r[14] = (pc + 4u) | 1u;
            *npc = (pc + 4u + (uint32_t)o) & ~1u;
            *costp = 3;
            return 1;"""
b = """            r[14] = (pc + 4u) | 1u;
            *npc = (pc + 4u + (uint32_t)o) & ~1u;
            /* Phase-5C: the suffix's bit 12 separates the forms - BL
             * (11111, bit12=1) stays in Thumb, BLX (11101, bit12=0) makes
             * the target ARM state. */
            smc_branch_mode(*npc | ((hw2 & 0x1000u) ? 1u : 0u));
            *costp = 3;
            return 1;"""
assert s.count(a) == 1
s = s.replace(a, b, 1)

a2 = """            *npc = (pc + 8u + (uint32_t)so) & ~1u;
            *costp = 3;"""
b2 = """            *npc = (pc + 8u + (uint32_t)so) & ~1u;
            smc_branch_mode(*npc);            /* Phase-5C: BLX is ARM state */
            *costp = 3;"""
assert s.count(a2) == 1
s = s.replace(a2, b2, 1)

a3 = """    thumb = nnk_pc_is_thumb(pc);
    rc = thumb ? smc_exec_thumb(pc, &npc, &cost) : smc_exec_arm(pc, w, &npc, &cost);"""
b3 = """    thumb = nnk_pc_is_thumb(pc);
    { int st; if (smc_carried_thumb(pc, &st)) thumb = st; }   /* Phase-5C */
    rc = thumb ? smc_exec_thumb(pc, &npc, &cost) : smc_exec_arm(pc, w, &npc, &cost);"""
assert s.count(a3) == 1
s = s.replace(a3, b3, 1)

a4 = """    thumb = nnk_pc_is_thumb(pc);
    if (!thumb) {
        int m = smc_state_from_regs(pc);
        if (m < 0) m = smc_t_sticky;
        thumb = (m > 0);
    }"""
b4 = """    thumb = nnk_pc_is_thumb(pc);
    { int st; if (smc_carried_thumb(pc, &st)) thumb = st; }   /* Phase-5C */
    if (!thumb) {
        int m = smc_state_from_regs(pc);
        if (m < 0) m = smc_t_sticky;
        thumb = (m > 0);
    }"""
assert s.count(a4) == 1
s = s.replace(a4, b4, 1)
open(p, 'w').write(s)
print('patched tools/nnk_smc.inc')
