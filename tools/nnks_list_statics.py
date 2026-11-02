#!/usr/bin/env python3
"""List the runtime static DATA objects a savestate must carry.

The savestate runtime (tools/nnk_state.inc) serializes every mutable static
object of the effective prelude: the frozen image's prelude up to run(), plus
every *.inc that nnk_main.c #includes before nnk_state.inc.  Enumerating them
by hand rots (a counter added later silently diverges the state), so this
script derives the list from the sources.

Handles multi-declarator statements (`static uint32_t a, b[4];`), trailing
comments, initializers (incl. brace/bracket initialisers), skips functions,
and SKIPS pointers / host handles: a state may never carry a host address
(ASLR, malloc layout, SDL/GL/FILE objects).  Those are re-derived or rebound by
the loader and are listed in nnk_state.inc's "NOT CARRIED" comment.

usage: tools/nnks_list_statics.py <image.c> <frag1.inc> [<frag2.inc> ...]
"""
import re
import sys

SKIP_NAMES = {'img', 'rom', 'smc_img', 'smc_seen_key', 'smc_seen_form', 'gxd',
              'gxd_shot', 'storewatch_fp', 'nnk_in_log', 'nnk_rec', 'p5t_buf',
              'smc_seg', 'p5t_str'}

IDENT = re.compile(r'[A-Za-z_][A-Za-z_0-9]*')


# The splitter un-static's exactly these DATA objects so the band TUs can see
# them (tools/split_dispatch.py prints the shared-symbol list at split time).
# They live at column 0 in nnk_main.c but are ordinary machine state.
SHARED_DATA = {'r', 'n', 'z', 'c', 'v', 'spsr_bank', 'bank13', 'bank14',
               'carry_out', 'cp15_reg', 'cpsr_ctl', 'mode_cur', 'b'}

TYPES = {'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t', 'int16_t',
         'int32_t', 'int64_t', 'int', 'unsigned', 'signed', 'long', 'short',
         'char', 'size_t', 'float', 'double', 'struct', 'union', 'enum'}


def strip_comments(line):
    for mark in ('/*', '//'):
        i = line.find(mark)
        if i >= 0:
            line = line[:i]
    return line


def split_declarators(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return out


def cut_initializer(d):
    depth = 0
    for i, ch in enumerate(d):
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == '=' and depth == 0:
            return d[:i]
    return d


def name_of(d):
    """Last identifier of a declarator, after dropping trailing [..] groups."""
    d = cut_initializer(d).rstrip()
    while True:
        m = re.search(r'\[[^\]]*\]\s*$', d)
        if not m:
            break
        d = d[:m.start()].rstrip()
    m = re.search(r'([A-Za-z_][A-Za-z_0-9]*)\s*$', d)
    if not m:
        return None, None, d
    return m.group(1), d[:m.start()], d


def scan(path, stop_at_run=True):
    names = []
    lines = open(path, errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        raw = strip_comments(lines[i]).rstrip()
        line = raw.strip()
        if stop_at_run and line.startswith('static void run(void)'):
            break
        i += 1
        if not line.startswith('static '):
            # The splitter un-static's exactly the SHARED objects so the band
            # TUs can see them: in nnk_main.c they are plain column-0
            # definitions.  They are machine state all the same, but a
            # column-0 line is otherwise just as likely to be a statement or a
            # local of an .inc fragment (nnk_dma.inc, nnk_smc.inc, ... are
            # pasted at column 0), so accept them only through the explicit
            # whitelist.  Every declarator of the statement is considered:
            # `uint32_t n, z, c, v;` and `bank13[6], bank14[6], spsr_bank[6]`
            # must each carry all three objects.
            if (not line or raw[:1].isspace() or '{' in line or '(' in line
                    or not line.endswith(';')):
                continue
            body = line.rstrip(';').strip()
            decls = split_declarators(body)
            base_typ = None
            for d in decls:
                name, typ, _ = name_of(d)
                if typ is None:
                    continue
                if not typ.split():
                    typ = base_typ or ''   # a later declarator inherits it
                else:
                    base_typ = typ
                if (not name or '*' in typ
                        or name not in SHARED_DATA):
                    continue
                if not typ.split() or typ.split()[0] not in TYPES:
                    continue      # #define / enum junk, not a definition
                names.append(name)
            continue
        if '(' in line.split('{')[0]:
            continue                      # function definition / prototype
        # A declaration whose body spans lines (anonymous struct/union or a
        # braced initializer) is accumulated up to its terminating ';'.
        stmt = line
        guard = 0
        while i < len(lines) and guard < 200:
            depth = stmt.count('{') - stmt.count('}')
            if depth <= 0 and stmt.rstrip().endswith(';'):
                break
            stmt += ' ' + strip_comments(lines[i]).strip()
            i += 1
            guard += 1
        if not stmt.rstrip().endswith(';'):
            continue
        body = stmt[len('static '):].rstrip().rstrip(';').strip()
        head = body
        if '{' in body and '}' in body:
            # keep only the declarator part after the struct/union body
            head = body[body.rfind('}') + 1:].strip()
            if head == '':
                continue                  # a pure type declaration
            full = head
        else:
            full = body
        decls = split_declarators(full)
        name, typ, _ = name_of(decls[0])
        if not name or typ is None:
            continue
        if typ.startswith('const '):
            typ = typ[len('const '):].strip()
        if '*' in typ:
            continue
        if name not in SKIP_NAMES:
            names.append(name)
        for d in decls[1:]:
            if d.strip().startswith('*'):
                continue
            n2, t2, _ = name_of(d)
            if n2 and n2 not in SKIP_NAMES:
                names.append(n2)
    return names


def main():
    seen = set()
    for f in sys.argv[1:]:
        for n in scan(f, stop_at_run=f.endswith('.c')):
            if n in seen:
                continue
            seen.add(n)
            print(n)


if __name__ == '__main__':
    main()
