#!/usr/bin/env python3
# Figure 1 (com-LiveJournal): end-to-end time of PimPam vs OFIS-PimPam over P1..P5 on 1,024
# DPUs, broken into phases. Two horizontal bars per pattern:
#   PimPam (solid) : read+sort | task assign | sg gen | sg xfer | match | res xfer
#   OFIS-PimPam (hatch) : read+sort | sg gen | match+xfer
# Reads breakdown.csv.
import os, csv, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "breakdown.csv")
OUT = os.path.join(HERE, "breakdown")

# P1..P5  (paper pattern ids -> kernel names)
PATS = [("P1", "HOUSE5"), ("P2", "HOUSE5_X"), ("P3", "K4_TAIL5"), ("P4", "P_P6"),
        ("P5", "HOUSE5_Y")]

# D[pid][engine] = row dict of phase times (ms). Missing/blank cells read as 0.
D = {}
for r in csv.DictReader(open(CSV)):
    D.setdefault(r["pid"], {})[r["engine"]] = r
def _f(row, k):
    v = row.get(k, "")
    return float(v) if v not in (None, "") else 0.0

def pimpam_seg(pid):
    r = D[pid]["PimPam"]
    return [_f(r, "read_sort"), _f(r, "task_assign"), _f(r, "sg_gen"),
            _f(r, "sg_xfer"), _f(r, "match"), _f(r, "res_xfer")], _f(r, "e2e")

def ofis_seg(pid):
    r = D[pid]["OFIS"]
    return [_f(r, "read_sort"), _f(r, "sg_gen"), _f(r, "match_xfer")], _f(r, "e2e")

# per-phase colours, chosen to step apart in greyscale luminance so segments stay
# separable in B&W; shared phases (read+sort, sg gen) reuse one colour across engines
C_READ  = "#d6d6d6"; C_TASK = "#204d78"; C_SGEN = "#7dbfe0"
C_SGX   = "#96551f"; C_MATCH = "#58a873"; C_RSX = "#5a1a1d"; C_MX = "#7a63ab"
PIM_C  = [C_READ, C_TASK, C_SGEN, C_SGX, C_MATCH, C_RSX]
OFIS_C = [C_READ, C_SGEN, C_MX]
matplotlib.rcParams["hatch.linewidth"] = 1.5
DIV = 1000.0   # ms -> s
HATCH = "////"                                  # OFIS-PimPam engine hatch

# keep only patterns whose PimPam+OFIS rows both exist in the CSV
def have(pid):
    return pid in D and "PimPam" in D[pid] and "OFIS" in D[pid]
missing = [pid for pid, ker in PATS if not have(pid)]
if missing:
    print(f"[fig1_cj] skipping (not in CSV): {', '.join(missing)}")
PATS = [pk for pk in PATS if have(pk[0])]

fig, ax = plt.subplots(figsize=(9.8, 5.1))
BH = 0.34; OFF = 0.20
for i, (pid, ker) in enumerate(PATS):
    ps, _ = pimpam_seg(pid)
    os_, _ = ofis_seg(pid)
    # PimPam -- upper bar of the group
    left = 0.0
    for v, c in zip(ps, PIM_C):
        ax.barh(i + OFF, v / DIV, height=BH, left=left / DIV, color=c,
                edgecolor="#222", linewidth=0.8)
        left += v
    # OFIS-PimPam -- lower bar of the group
    left = 0.0
    for v, c in zip(os_, OFIS_C):
        ax.barh(i - OFF, v / DIV, height=BH, left=left / DIV, color=c,
                edgecolor="#222", linewidth=0.8, hatch=HATCH)
        left += v

ax.set_yticks(range(len(PATS)))
ax.set_yticklabels([p for p, _ in PATS], fontsize=19)
ax.set_xlabel("execution time(sec)", fontsize=22)
ax.tick_params(axis="x", labelsize=18)
ax.margins(y=0.06)
for s in ("top", "right", "left"):
    ax.spines[s].set_visible(False)

# legend: two rows grouped by engine, PimPam (6 phases) above OFIS-PimPam (3),
# columns aligned; the shorter OFIS row is padded with blanks
blank = Patch(fc="none", ec="none", label="")
pim_handles = [
    Patch(fc=C_READ,  ec="#222", lw=0.8, label="read+sort"), Patch(fc=C_TASK,  ec="#222", lw=0.8, label="task assign"),
    Patch(fc=C_SGEN,  ec="#222", lw=0.8, label="sg gen"), Patch(fc=C_SGX, ec="#222", lw=0.8, label="sg xfer"),
    Patch(fc=C_MATCH, ec="#222", lw=0.8, label="match"),     Patch(fc=C_RSX,  ec="#222", lw=0.8, label="res xfer"),
]
ofis_handles = [
    Patch(fc=C_READ, ec="#222", lw=0.8, hatch=HATCH, label="read+sort"), blank,
    Patch(fc=C_SGEN, ec="#222", lw=0.8, hatch=HATCH, label="sg gen"),
    Patch(fc=C_MX,   ec="#222", lw=0.8, hatch=HATCH, label="match+xfer"), blank, blank,
]
LABEL_X = -0.04
LX = 0.145
Y_PIM, Y_OFIS = 1.140, 1.028
# each engine label sits at the same Y as its legend row (va="center")
lblkw = dict(transform=ax.transAxes, fontsize=17, va="center", ha="left")
legkw = dict(ncol=6, fontsize=17, frameon=False,
             loc="center left", handlelength=1.0, columnspacing=0.6, handletextpad=0.3)
ax.text(LABEL_X, Y_PIM, "PimPam", **lblkw)
leg1 = ax.legend(handles=pim_handles, bbox_to_anchor=(LX, Y_PIM), **legkw)
ax.add_artist(leg1)
ax.text(LABEL_X, Y_OFIS, "OFIS-PimPam", **lblkw)
ax.legend(handles=ofis_handles, bbox_to_anchor=(LX, Y_OFIS), **legkw)

plt.tight_layout()
for ext in ("png", "pdf", "eps"):
    fig.savefig(f"{OUT}.{ext}", dpi=300, bbox_inches="tight")
print(f"wrote {OUT}.{{png,pdf,eps}}")
