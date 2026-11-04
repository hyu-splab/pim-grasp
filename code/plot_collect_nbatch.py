#!/usr/bin/env python3
# Companion to plot_collect_mean.py: total number of result-collection transfers per graph
# (= total task batches for per-batch collection). Reads collect_calls from collect.csv,
# else parses "calls=" from the matching collectraw_<G>_PER_*.log.
import os, csv, glob, re, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3

HERE = os.path.dirname(os.path.abspath(__file__))
RESULT = os.path.join(HERE, "..", "result")
OUT  = os.path.join(HERE, "collect_nbatch")
DATASETS = [g for g in ["PT", "CJ", "PK", "LJ"] if g in os.environ.get("GRAPHS", "PT CJ PK LJ").split()]

# newest CSV that has all 4 cells
def _newest_complete():
    for c in sorted(glob.glob(os.path.join(RESULT, "collectcmp_HOUSE5_*.csv")), reverse=True):
        seen = {(r["cell"], r["mode"]) for r in csv.DictReader(open(c))}
        if all((g, m) in seen for g in DATASETS for m in ("PER", "FR")):
            return c
    raise SystemExit("no complete collectcmp CSV found")
CSV = os.path.join(HERE, "collect.csv")
NR_DPUS = 1024

def per_calls_from_log(cell):
    stamp = re.search(r"(\d{8}-\d{6})", os.path.basename(CSV)).group(1)
    logs = glob.glob(os.path.join(RESULT, f"collectraw_{cell}_PER_{stamp}*.log"))
    for lg in sorted(logs):
        for ln in open(lg):
            m = re.search(r"^\s*collect .*calls=(\d+)", ln)
            if m:
                return float(m.group(1))
    return float("nan")

# per-batch #collects (= total task batches); prefer a collect_calls column, else the log
rows = list(csv.DictReader(open(CSV)))
have_col = rows and "collect_calls" in rows[0]
per = []
for g in DATASETS:
    if have_col:
        v = [r["collect_calls"] for r in rows if r["cell"] == g and r["mode"] == "PER"]
        per.append(float(v[0]) if v and v[0] not in ("", "?", "NA") else per_calls_from_log(g))
    else:
        per.append(per_calls_from_log(g))
C_PER = "#a9c0d6"   # matches plot_collect_mean.py's per-batch bar
x = np.arange(len(DATASETS)); w = 0.52
FX, FY, FL = 25, 26, 25

fig, ax = plt.subplots(figsize=(6.4, 4.4), constrained_layout=True)
ax.bar(x, per, w, color=C_PER, edgecolor="#222", linewidth=0.9)

ax.set_xticks(x); ax.set_xticklabels(DATASETS, fontsize=FX)
ax.set_ylabel(r"No. of batches ($\times 10^5$)", fontsize=24)
ax.tick_params(axis="y", labelsize=FY, which="both")
# unit factored into the ylabel so ticks are plain integers
from matplotlib.ticker import MultipleLocator, FuncFormatter
ax.set_ylim(0, max(1.45e6, max(per) * 1.05))   # never clip a measured bar
ax.yaxis.set_major_locator(MultipleLocator(4e5))
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, p: f"{v/1e5:.0f}"))
for s in ("top", "right", "bottom", "left"):
    ax.spines[s].set_visible(True); ax.spines[s].set_linewidth(1.0)

for ext in ("png", "pdf", "eps"):
    fig.savefig(f"{OUT}.{ext}", dpi=300, bbox_inches="tight")
print(f"wrote {OUT}.{{png,pdf,eps}}  per={[int(p) for p in per]}  (from {os.path.basename(CSV)})")
