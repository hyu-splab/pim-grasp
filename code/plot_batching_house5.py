#!/usr/bin/env python3
# Effect of dynamic task batching (HOUSE5), as two single-panel figures:
#   batching_xfer   -- subgraph-transfer time (sec)
#   batching_nbatch -- number of task batches
# Compares no-batch vs +batch over the four graphs. Reads the newest result/expbatch_*.csv.
import os, sys, csv, glob, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3

HERE = os.path.dirname(os.path.abspath(__file__))
CSV  = sorted(glob.glob(os.path.join(HERE, "..", "result", "expbatch_*.csv")))[-1]
FIGDIR = os.path.join(HERE, "..", "sections", "figures")
PATTERN = "HOUSE5"
DATASETS = [g for g in ["PT", "CJ", "PK", "LJ"] if g in os.environ.get("GRAPHS", "PT CJ PK LJ").split()]

# d[ds][cfg] = (subgraph_xfer_ms, result_collect_ms, batches)
d = {ds: {} for ds in DATASETS}
for r in csv.DictReader(open(CSV)):
    if r["pattern"] != PATTERN or r["cell"] not in d:
        continue
    d[r["cell"]][r["config"]] = (float(r["xfer_total_ms"]),
                                 float(r.get("collect_total_ms") or 0.0),
                                 float(r["batches_total"]))

xfer1 = [d[ds]["C1"][0] / 1000.0 for ds in DATASETS]   # subgraph transfer, ms -> sec
xfer2 = [d[ds]["C3"][0] / 1000.0 for ds in DATASETS]
col1  = [d[ds]["C1"][1] / 1000.0 for ds in DATASETS]   # result transfer, ms -> sec
col2  = [d[ds]["C3"][1] / 1000.0 for ds in DATASETS]
nb1   = [d[ds]["C1"][2] for ds in DATASETS]            # task batches
nb2   = [d[ds]["C3"][2] for ds in DATASETS]

if sum(nb1) + sum(nb2) == 0:
    sys.exit(f"ERROR: {os.path.basename(CSV)} has no batch counts; batching figures not generated")

from matplotlib.patches import Patch
matplotlib.rcParams["hatch.linewidth"] = 1.6
# colour = batching (w/o vs w/); hatch tells subgraph transfer (solid) from result transfer (hatched)
C_WO  = "#dd8452"   # w/o batching
C_W   = "#2f4b7c"   # w/  batching
C_NO  = C_WO; C_BT = C_W
# two bars per group sit flush: BW == 2*SEP
x = np.arange(len(DATASETS)); SEP = 0.20; BW = 0.40; w = BW
FL, FX, FY, FLG = 26, 26, 23, 23   # ylabel / xtick / ytick / legend

fig, ax = plt.subplots(figsize=(6.4, 4.6), constrained_layout=True)
for xx, col, sg, rs in [(x - SEP, C_WO, xfer1, col1),
                        (x + SEP, C_W,  xfer2, col2)]:
    ax.bar(xx, sg, w, color=col, edgecolor="#222", linewidth=0.9)              # subgraph transfer
    ax.bar(xx, rs, w, bottom=sg, color=col, edgecolor="#222", linewidth=0.9)   # result transfer
    ax.bar(xx, rs, w, bottom=sg, color="none", edgecolor="white",             # white hatch overlay
           linewidth=0.0, hatch="///")

ax.margins(y=0.16)
ax.set_xticks(x); ax.set_xticklabels(DATASETS, fontsize=FX)
ax.set_ylabel("Data transfer time (sec)", fontsize=FL)
ax.tick_params(axis="y", labelsize=FY)
# legend order matches the stack (top->bottom): result transfer then subgraph transfer
leg = [Patch(fc="white", ec="#222", hatch="////", label="result transfer"),
       Patch(fc="white", ec="#222", label="subgraph transfer")]
ax.legend(handles=leg, fontsize=FLG, frameon=False, loc="upper left",
          handlelength=1.3, handletextpad=0.4, labelspacing=0.3)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
for ext in ("png", "pdf", "eps"):
    fig.savefig(os.path.join(FIGDIR, f"batching_xfer.{ext}"), dpi=300, bbox_inches="tight")
plt.close(fig)

# ---- number of task batches (x10^6): w/o vs w/ batching ----
nb1m = [v / 1e6 for v in nb1]
nb2m = [v / 1e6 for v in nb2]
figb, axb = plt.subplots(figsize=(6.4, 4.6), constrained_layout=True)
axb.bar(x - SEP, nb1m, BW, color=C_NO, edgecolor="#222", linewidth=0.9, label="w/o task batching")
axb.bar(x + SEP, nb2m, BW, color=C_BT, edgecolor="#222", linewidth=0.9, label="w/ task batching")
axb.margins(y=0.18)
axb.set_xticks(x); axb.set_xticklabels(DATASETS, fontsize=FX)
axb.set_ylabel(r"No. of batches ($\times 10^{6}$)", fontsize=FL)
axb.tick_params(axis="y", labelsize=FY, which="both")
axb.legend(fontsize=FLG, frameon=False, loc="upper left", ncol=1,
           handlelength=1.3, handletextpad=0.4, labelspacing=0.3, borderaxespad=0.6)
for s in ("top", "right"):
    axb.spines[s].set_visible(False)
for ext in ("png", "pdf", "eps"):
    figb.savefig(os.path.join(FIGDIR, f"batching_nbatch.{ext}"), dpi=300, bbox_inches="tight")
plt.close(figb)
print(f"wrote {FIGDIR}/batching_xfer.* and batching_nbatch.*  (from {os.path.basename(CSV)})")
