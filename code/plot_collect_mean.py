#!/usr/bin/env python3
# Result-collection time (per-rank mean, ms) for P1 (HOUSE5) across PT/PK/CJ/LJ:
# per-batch collect vs batched (final-retire) collect. Log y-axis (the two differ by 200-1000x).
import os, csv, glob, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3
matplotlib.rcParams["hatch.linewidth"] = 1.2

HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "collect_compare")
DATASETS = [g for g in ["PT", "CJ", "PK", "LJ"] if g in os.environ.get("GRAPHS", "PT CJ PK LJ").split()]

# newest CSV that has all 4 cells x both modes
def newest_complete_csv():
    for c in sorted(glob.glob(os.path.join(HERE, "..", "result", "collectcmp_HOUSE5_*.csv")), reverse=True):
        seen = {(r["cell"], r["mode"]) for r in csv.DictReader(open(c))}
        if all((g, m) in seen for g in DATASETS for m in ("PER", "FR")):
            return c
    raise SystemExit("no complete collectcmp CSV found")
CSV = os.path.join(HERE, "collect.csv")

# d[cell][mode] = mean collect ms
d = {g: {} for g in DATASETS}
for r in csv.DictReader(open(CSV)):
    if r["cell"] in d:
        d[r["cell"]][r["mode"]] = float(r["collect_mean_ms"])

per = [d[g]["PER"] for g in DATASETS]
fr  = [d[g]["FR"]  for g in DATASETS]
fold = [p / f for p, f in zip(per, fr)]

# steel-blue two-tone, B&W-separable
C_PER = "#a9c0d6"   # per-batch collection
C_FR  = "#39627d"   # batched collection
x = np.arange(len(DATASETS)); w = 0.40
FX, FY, FL, FLG = 25, 26, 25, 22

fig, ax = plt.subplots(figsize=(6.4, 4.4), constrained_layout=True)
ax.set_yscale("log")
b1 = ax.bar(x - w/2, per, w, color=C_PER, edgecolor="#222", linewidth=0.9, label="per-batch collection")
b2 = ax.bar(x + w/2, fr,  w, color=C_FR,  edgecolor="#222", linewidth=0.9, label="batched collection")

ax.set_xticks(x); ax.set_xticklabels(DATASETS, fontsize=FX)
ax.set_ylabel("collection time (ms)", fontsize=24)
ax.tick_params(axis="y", labelsize=FY, which="both")
ax.set_ylim(3 if min(fr) >= 3 else min(fr) / 1.5, max(min(max(per) * 9, 9e4), max(per) * 1.3))   # keep top below 10^5 unless a bar needs more
from matplotlib.ticker import LogLocator, NullLocator
ax.yaxis.set_major_locator(LogLocator(base=10, numticks=15))   # label every decade
ax.yaxis.set_minor_locator(NullLocator())
ax.legend(fontsize=FLG, frameon=False, loc="upper left", ncol=1,
          handlelength=1.3, handletextpad=0.4, labelspacing=0.3, borderaxespad=0.6)
for s in ("top", "right", "bottom", "left"):
    ax.spines[s].set_visible(True); ax.spines[s].set_linewidth(1.0)

for ext in ("png", "pdf", "eps"):
    fig.savefig(f"{OUT}.{ext}", dpi=300, bbox_inches="tight")
print(f"wrote {OUT}.{{png,pdf,eps}}  (from {os.path.basename(CSV)})")
