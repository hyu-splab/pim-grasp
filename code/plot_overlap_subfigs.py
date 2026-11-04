#!/usr/bin/env python3
# Overlap effect as four per-dataset panels, each a standalone .eps for a subfigure.
# One panel per dataset (PT/PK/CJ/LJ), x = P1..P5, two bars per pattern: serialized vs
# overlapped. Legend only on the PT panel. Reads result/expoverlap_full.csv.
import os, csv, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import MaxNLocator

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3
matplotlib.rcParams["hatch.linewidth"] = 1.3

HERE = os.path.dirname(os.path.abspath(__file__))
CSV  = os.path.join(HERE, "..", "result", "expoverlap_full.csv")
FIGDIR = os.path.join(HERE, "..", "sections", "figures")
DATASETS = [g for g in ["PT", "PK", "CJ", "LJ"] if g in os.environ.get("GRAPHS", "PT CJ PK LJ").split()]
PIDS = ["P1", "P2", "P3", "P4", "P5"]

d = {g: {} for g in DATASETS}
for r in csv.DictReader(open(CSV)):
    if r["cell"] in d:
        d[r["cell"]][r["pid"]] = (float(r["e2e_off_ms"]) / 1000.0,
                                  float(r["e2e_on_ms"]) / 1000.0)

# two colours distinct in B&W: light green vs dark purple
C_NO = "#a1d99b"   # serialized
C_BT = "#54278f"   # overlapped
x = np.arange(len(PIDS)); w = 0.40
FT, FX, FY, FL, FLG = 22, 18, 21, 21, 18   # FT=panel title, FX=xtick, FY/FL=y-axis, FLG=legend
LW = 1.0

# all four panels share the same canvas and margins; y-label only on the left column
LEFTCOL = ("PT", "PK")
for g in DATASETS:
    off = [d[g][p][0] for p in PIDS]
    on  = [d[g][p][1] for p in PIDS]
    leftm = 0.205
    fig, ax = plt.subplots(figsize=(5.2, 3.98))
    fig.subplots_adjust(left=leftm, right=0.968, top=0.951, bottom=0.195)
    ax.bar(x - w/2, off, w, color=C_NO, edgecolor="#222", linewidth=LW, label="serialized")
    ax.bar(x + w/2, on,  w, color=C_BT, edgecolor="#222", linewidth=LW, label="overlapped")
    ax.text(0.97, 0.95, g, transform=ax.transAxes, ha="right", va="top", fontsize=20)  # dataset name inside top-right
    ax.set_xticks(x); ax.set_xticklabels(PIDS, fontsize=FX)
    if g in LEFTCOL:
        ax.set_ylabel("E2E time (sec)", fontsize=FL)
    ax.tick_params(axis="y", labelsize=FY)
    ax.yaxis.set_major_locator(MaxNLocator(nbins=6, steps=[1, 2, 5, 10]))
    ax.margins(y=0.62 if g == "PT" else 0.12)   # extra PT headroom for the legend
    if g == "PT":   # single shared legend on PT
        ax.legend(fontsize=FLG, frameon=False, loc="upper left",
                  ncol=1, handlelength=1.1, handletextpad=0.4,
                  labelspacing=0.3, borderaxespad=0.5)
    for s in ("top", "right", "bottom", "left"):
        ax.spines[s].set_visible(True); ax.spines[s].set_linewidth(1.0)
    out = os.path.join(FIGDIR, f"overlap_{g}")
    for ext in ("png", "pdf", "eps"):
        fig.savefig(f"{out}.{ext}", dpi=300)   # no tight bbox -> identical canvas size
    plt.close(fig)
    print(f"wrote {out}.{{png,pdf,eps}}")
