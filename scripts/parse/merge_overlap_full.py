#!/usr/bin/env python3
# Merge the full overlap experiment (P1..P5 x PT/PK/CJ/LJ) onto one CSV.
#   w/o overlap  <- newest result/expoverlapwo_*.csv  (scripts/run/exp_overlap_wo.sh; bulk prebuild)
#   w/  overlap  <- a barrier-hybrid PIM-grasp E2E, pulled from a breakdown CSV (the "on" arm).
# Also cross-checks each (cell,pattern) ans for correctness.
#   reads : <bundle>/result/expoverlapwo_*.csv  (all of them; later files win per cell/pattern)
#           <bundle>/result/breakdown_*/breakdown.csv  (newest; env B_CSV=<path> to override)
#   writes: <bundle>/data/expoverlap_full.csv   (consumed by code/plot_overlap_subfigs.py)
#   python3 scripts/parse/merge_overlap_full.py
import os, csv, glob, sys
HERE   = os.path.dirname(os.path.abspath(__file__))
BUNDLE = os.path.abspath(os.path.join(HERE, "..", ".."))
RES    = os.path.join(BUNDLE, "result")
OUT    = os.path.join(BUNDLE, "data")
os.makedirs(OUT, exist_ok=True)

PATS = [("P1", "HOUSE5"), ("P2", "HOUSE5_X"), ("P3", "K4_TAIL5"),
        ("P4", "P_P6"), ("P5", "HOUSE5_Y")]
DATASETS = ["PT", "PK", "CJ", "LJ"]

wo_csvs = sorted(glob.glob(os.path.join(RES, "expoverlapwo_*.csv")))
if not wo_csvs:
    sys.exit("no result/expoverlapwo_*.csv (run scripts/run/exp_overlap_wo.sh first)")
B_CSV = os.environ.get("B_CSV")
if not B_CSV:
    bds = sorted(glob.glob(os.path.join(RES, "breakdown_*", "breakdown.csv")))
    if not bds:
        sys.exit("no result/breakdown_*/breakdown.csv for the w/ overlap arm "
                 "(run scripts/run/run_breakdown.sh, or set B_CSV=<path>)")
    B_CSV = bds[-1]

# w/o overlap: (cell,pattern) -> (e2e_wo_ms, total_build_ms, ans)
wo = {}
for fn in wo_csvs:
    for r in csv.DictReader(open(fn)):
        wo[(r["cell"], r["pattern"])] = (float(r["e2e_wo_ms"]),
                                         float(r["total_build_ms"]), r["ans"])

# w/ overlap: (cell,pattern) -> (e2e_on_ms, ans) from the breakdown pimgrasp rows
on = {}
for r in csv.DictReader(open(B_CSV)):
    if r["engine"] == "pimgrasp" and r.get("e2e_ms") and r.get("dpus") == "1024":
        on[(r["graph"], r["pattern"])] = (float(r["e2e_ms"]), r.get("ans", ""))

out = os.path.join(OUT, "expoverlap_full.csv")
with open(out, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["cell", "pid", "pattern", "e2e_off_ms", "e2e_on_ms",
                "total_build_ms", "saved_ms", "speedup", "ans_match"])
    print(f"wo <- {', '.join(os.path.basename(x) for x in wo_csvs)}")
    print(f"on <- {os.path.relpath(B_CSV, BUNDLE)}")
    for pid, ker in PATS:
        for g in DATASETS:
            wr = wo.get((g, ker)); orr = on.get((g, ker))
            if not wr or not orr:
                print(f"  MISSING {g}/{ker}"); continue
            e_off, build, ans_wo = wr
            e_on, ans_on = orr
            saved = e_off - e_on
            spd = e_off / e_on if e_on else 0.0
            match = "OK" if ans_wo == ans_on else f"MISMATCH({ans_wo}/{ans_on})"
            w.writerow([g, pid, ker, f"{e_off:.3f}", f"{e_on:.3f}",
                        f"{build:.3f}", f"{saved:.3f}", f"{spd:.3f}", match])
            print(f"  {g}/{pid}  off={e_off:.1f} on={e_on:.1f} saved={saved:.1f} {match}")
print(f"\nwrote {out}")
