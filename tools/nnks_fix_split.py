p = 'tools/split_dispatch.py'
s = open(p).read()

old_hook = ('    prelude = inject(prelude, "        arm7_evt_tick();",\n'
            '                     "        arm7_evt_tick();\\n        nnks_tick();",\n'
            '                     "Phase-5AQ savestate per-step hook")')
new_hook = ('    prelude = inject(prelude, "        arm7_evt_tick();",\n'
            '                     "        arm7_evt_tick();\\n        nnks_tick(pc);",\n'
            '                     "Phase-5AQ savestate per-step hook")\n'
            '    # The restored state must resume at the PC it was saved at, so run()\n'
            '    # takes its entry PC from the state module (ENTRY_CONST unless a\n'
            '    # state was loaded).  nnks_tick(pc) above keeps that mirror current.\n'
            '    prelude = inject(prelude, "    uint32_t pc = ENTRY_CONST;",\n'
            '                     "    uint32_t pc = nnks_entry_pc();",\n'
            '                     "Phase-5AQ resume PC")')
assert s.count(old_hook) == 1, ('hook', s.count(old_hook))
s = s.replace(old_hook, new_hook)
open(p, 'w').write(s)
print('ok')
