import re
p = 'tools/nnks_list_statics.py'
s = open(p).read()

old = """            if typ.split()[0] not in TYPES:
                continue          # #define / enum junk, not a definition
"""
new = """            if not typ.split() or typ.split()[0] not in TYPES:
                continue          # #define / enum junk, not a definition
            if name not in SHARED_DATA:
                continue          # not one of the un-static'd shared objects
"""
old2 = """            if not typ.split() or typ.split()[0] not in TYPES:
                continue          # #define / enum junk, not a definition
            if name not in SHARED_DATA:
"""
# the previous sed already added the "not typ.split()" guard; normalise first
s = s.replace(old2, old2)
if old in s:
    s = s.replace(old, new, 1)

shared = """
# The splitter un-static's exactly these DATA objects so the band TUs can see
# them (tools/split_dispatch.py prints the shared-symbol list at split time).
# They live at column 0 in nnk_main.c but are ordinary machine state.
SHARED_DATA = {'r', 'n', 'z', 'c', 'v', 'spsr_bank', 'carry_out', 'cp15_reg',
               'cpsr_ctl', 'mode_cur', 'b'}
"""
if 'SHARED_DATA =' not in s:
    s = s.replace('TYPES = {', shared + '\nTYPES = {', 1)
open(p, 'w').write(s)
print('SHARED_DATA' in s)
