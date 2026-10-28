#!/usr/bin/env python3
"""Compare the first N guest-PC trace lines of the recompiled ARM9 entry
against the addresses listed in work/entry_disasm_check.txt.

Usage: check_entry_trace.py TRACE.txt CHECK.txt [N]
Exit 0 when the first N trace guest PCs match the first N check addresses.
"""
import re
import sys

TRACE_PC = re.compile(r"^(0x[0-9a-fA-F]{8})$")
CHECK_LINE = re.compile(r"^(\d+)\s+(0x[0-9a-fA-F]{8})\s")


def main() -> int:
    trace_path, check_path = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 64

    trace = []
    with open(trace_path) as fh:
        for line in fh:
            m = TRACE_PC.match(line.strip())
            if m:
                trace.append(int(m.group(1), 16))

    check = []
    with open(check_path) as fh:
        for line in fh:
            m = CHECK_LINE.match(line)
            if m:
                check.append(int(m.group(2), 16))

    print("trace guest PCs parsed : %d" % len(trace))
    print("check addresses parsed : %d" % len(check))
    want = check[:n]
    got = trace[: len(want)]
    ok = sum(1 for a, b in zip(want, got) if a == b)
    print("first %d entries compared: %d match" % (len(want), ok))
    if ok != len(want):
        print("mismatches:")
        for i, (a, b) in enumerate(zip(want, got)):
            if a != b:
                print("  [%d] check=0x%08X trace=0x%08X" % (i, a, b))
        for i in range(len(got), len(want)):
            print("  [%d] check=0x%08X trace=<missing>" % (i, want[i]))
        return 1
    print("RESULT: PASS — first %d guest-PC trace entries match %s" % (len(want), check_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
