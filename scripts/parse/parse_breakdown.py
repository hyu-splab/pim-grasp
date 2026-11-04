#!/usr/bin/env python3
# Turn one run_breakdown.sh CSV into the inputs of Figures 1, 8, 9, 10, and 11, so these figures
# (and the "with overlap" arm of Figure 15) show the same measurements.
#   reads : a result/breakdown_<stamp>/breakdown.csv (argv 1)
#   writes: <bundle>/data/breakdown_overall.csv, breakdown_dpusweep.csv  (fastest run per point)
#           <bundle>/data/overall_p14_<graph>.dat                        (Figure 8: E2E in seconds)
#           <bundle>/data/breakdown_fig1cj.csv                           (Figure 1: CJ phases in ms)
#           <bundle>/data/dpu_speedup_<graph>.dat                        (Figure 10: PimPam/PIM-grasp)
# An output is written only when all of its points were measured, with matching answers.
# Otherwise the missing points are reported and the script exits non-zero.
import csv, glob, os, sys

BUNDLE = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DATA = os.path.join(BUNDLE, "data")
PATS = [("P1", "HOUSE5"), ("P2", "HOUSE5_X"), ("P3", "K4_TAIL5"), ("P4", "P_P6"), ("P5", "HOUSE5_Y")]
ENGINES = ["pimpam", "ofis", "pimgrasp"]
HEADLINE_DPUS = "1024"
SWEEP_DPUS = ["64", "128", "256", "512", "1024"]
SWEEP_GRAPHS = ["PT", "LJ"]

if len(sys.argv) != 2:
    sys.exit("usage: parse_breakdown.py <breakdown.csv>")
SRC = sys.argv[1]

with open(SRC) as f:
    reader = csv.DictReader(f)
    fields = reader.fieldnames
    raw = list(reader)

# Fastest successful run per (engine, graph, pattern, dpus); file order is kept.
best = {}
for r in raw:
    if r["status"] not in ("fine", "ok") or not r["e2e_ms"]:
        continue
    k = (r["engine"], r["graph"], r["pattern"], r["dpus"])
    if k not in best or float(r["e2e_ms"]) < float(best[k]["e2e_ms"]):
        best[k] = r

os.makedirs(DATA, exist_ok=True)
for name in ("breakdown_overall.csv", "breakdown_dpusweep.csv"):
    with open(os.path.join(DATA, name), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(best.values())

# Remove older outputs: every figure must come from this CSV.
for pat in ("overall_p14_*.dat", "dpu_speedup_*.dat", "breakdown_fig1cj.csv"):
    for old in glob.glob(os.path.join(DATA, pat)):
        os.remove(old)

src_name = os.path.basename(os.path.dirname(os.path.abspath(SRC)))
errors = []
for g in sorted({r["graph"] for r in raw}):
    rows, bad = [], []
    for pid, pat in PATS:
        cell = [best.get((e, g, pat, HEADLINE_DPUS)) for e in ENGINES]
        if not all(cell):
            miss = [e for e, c in zip(ENGINES, cell) if not c]
            bad.append(f"{g}/{pat}: no valid run for {', '.join(miss)}")
            continue
        answers = {c["ans"] for c in cell}
        if len(answers) != 1:
            bad.append(f"{g}/{pat}: answers differ ({', '.join(sorted(answers))})")
            continue
        rows.append(f"{pid} " + " ".join(f"{float(c['e2e_ms']) / 1000:.3f}" for c in cell))
    if bad:
        errors += bad
        continue
    with open(os.path.join(DATA, f"overall_p14_{g}.dat"), "w") as f:
        f.write(f"# pattern  PimPam  OFIS-PimPam  PIM-grasp   (end_to_end_sec)   src: {src_name}\n")
        f.write("\n".join(rows) + "\n")

# Figure 1: PimPam and OFIS-PimPam phases on CJ. sg_gen (PimPam) and match_xfer (OFIS-PimPam) are
# the residuals, so each bar sums to the measured E2E.
num = lambda r, k: float(r[k] or 0.0)
fig1 = []
if any(r["graph"] == "CJ" for r in raw):
    for pid, pat in PATS:
        pp, of = (best.get((e, "CJ", pat, HEADLINE_DPUS)) for e in ("pimpam", "ofis"))
        if not (pp and of):
            errors.append(f"CJ/{pat}: no valid PimPam/OFIS-PimPam run for Figure 1")
            continue
        read, task, sgx, match, rsx, e2e = (num(pp, k) for k in
            ("readsort_ms", "task_ms", "xfer_ms", "exec_ms", "collect_ms", "e2e_ms"))
        fig1.append(dict(pid=pid, pattern=pat, engine="PimPam", read_sort=read, task_assign=task,
                         sg_gen=max(e2e - read - task - sgx - match - rsx, 0.0), sg_xfer=sgx,
                         match=match, res_xfer=rsx, match_xfer="", e2e=e2e))
        read, sgen, e2e = (num(of, k) for k in ("readsort_ms", "comp_exposed_ms", "e2e_ms"))
        fig1.append(dict(pid=pid, pattern=pat, engine="OFIS", read_sort=read, task_assign="",
                         sg_gen=sgen, sg_xfer="", match="", res_xfer="",
                         match_xfer=max(e2e - read - sgen, 0.0), e2e=e2e))
    if len(fig1) == 2 * len(PATS):
        with open(os.path.join(DATA, "breakdown_fig1cj.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(fig1[0]))
            w.writeheader()
            w.writerows(fig1)

# Figure 10: PimPam / PIM-grasp E2E per DPU count, for a graph whose sweep was run.
for g in SWEEP_GRAPHS:
    if not any(r["graph"] == g and r["dpus"] != HEADLINE_DPUS for r in raw):
        continue
    rows, ok = [], True
    for d in SWEEP_DPUS:
        cells = []
        for _, pat in PATS:
            pp, pg = (best.get((e, g, pat, d)) for e in ("pimpam", "pimgrasp"))
            if not (pp and pg):
                errors.append(f"{g}/{pat}/{d} DPUs: no valid PimPam/PIM-grasp run for Figure 10")
                ok = False
                continue
            cells.append(f"{float(pp['e2e_ms']) / float(pg['e2e_ms']):.4f}")
        rows.append(f"{d}  " + "  ".join(cells))
    if ok:
        with open(os.path.join(DATA, f"dpu_speedup_{g}.dat"), "w") as f:
            f.write("# dpus  " + "  ".join(pid for pid, _ in PATS) + "\n")
            f.write("\n".join(rows) + "\n")

if errors:
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    sys.exit(1)
print(f"wrote the inputs of Figures 1, 8, 9, 10, 11 to data/ from {SRC}")
