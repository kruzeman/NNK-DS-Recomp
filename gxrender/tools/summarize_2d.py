import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
wins = [(1200,1250),(3500,3550),(6000,6050),(9000,9050),(12000,12050),
        (14250,14510),(15100,15150),(16200,16250),(19590,19740),(21000,21050)]
for a,b in wins:
    sel = [r for r in rows if a <= int(r['unit']) <= b]
    if not sel:
        print(f"{a}-{b}: MISSING"); continue
    dp = sum(int(r['diffpx']) for r in sel)
    mx = max(int(r['maxdelta']) for r in sel)
    exact = sum(float(r['exact_pct']) for r in sel)/len(sel)
    worst = max(sel, key=lambda r: int(r['diffpx']))
    print(f"{a}-{b}: units={len(sel)} diffpx={dp} maxdelta={mx} "
          f"mean_exact={exact:.4f}% worst_unit={worst['unit']}({worst['diffpx']})")
