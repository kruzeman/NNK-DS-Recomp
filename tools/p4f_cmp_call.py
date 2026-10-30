#!/usr/bin/env python3
"""Phase-4F: diff the *same* 0x0200C638 call (the one that returns to the first
0x0210E380) between a melonDS PC trace and a native PC trace."""
import sys

mel_path, nat_path = sys.argv[1], sys.argv[2]
LO, HI = 0x0200C5D8, 0x0200CA00


def runs_before_anchor(path, anchor):
    """All [0x0200C638 ... ] in-range PC runs seen before `anchor` executes."""
    runs = []
    cur = None
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s.startswith("0x"):
                continue
            pc = int(s, 16)
            if pc == anchor:
                break
            if pc == 0x0200C638:
                cur = [s]
            elif cur is not None:
                if LO <= pc < HI:
                    cur.append(s)
                else:
                    runs.append(cur)
                    cur = None
    if cur:
        runs.append(cur)
    return runs


mel_runs = runs_before_anchor(mel_path, 0x0210E380)
nat_runs = runs_before_anchor(nat_path, 0x0210E380)
print(f"0x0200C638 calls before the first 0x0210E380: melonDS {len(mel_runs)}  native {len(nat_runs)}")
if not mel_runs or not nat_runs:
    sys.exit("no call found")
mel, nat = mel_runs[-1], nat_runs[-1]
print(f"comparing call #{len(mel_runs)} (melonDS, {len(mel)} PCs) vs call #{len(nat_runs)} (native, {len(nat)} PCs)")
k = 0
while k < min(len(mel), len(nat)) and mel[k] == nat[k]:
    k += 1
print(f"common prefix inside the call: {k}")
if k < len(mel) and k < len(nat):
    print(f"first difference at #{k}: melonDS {mel[k]}  native {nat[k]}")
    lo = max(0, k - 14)
    print("melonDS:", " ".join(mel[lo:k + 8]))
    print("native :", " ".join(nat[lo:k + 8]))
elif k == len(nat):
    print("native path is a strict prefix; melonDS continues:", " ".join(mel[k:k + 10]))
else:
    print("melonDS path is a strict prefix; native continues:", " ".join(nat[k:k + 10]))
