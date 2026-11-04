#!/usr/bin/env python3
# Phase breakdown vs DPU count (64..1024), two engines with different phase sets:
#   PimPam    (solid)  : read+sort | task assign | sg gen | sg xfer | match | res xfer
#   PIM-grasp (hatched): read+sort | sg gen+match+xfer
# One figure for the paper's per-DPU breakdown (P1=HOUSE5). read+sort/task-assign from the
# run_breakdown CSV.
#   python3 code/plot_breakdown_dpusweep.py [breakdown_dir]
import os, sys, csv, glob, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Liberation Sans", "Arial", "Helvetica", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42
matplotlib.rcParams["hatch.linewidth"] = 1.0

HERE = os.path.dirname(os.path.abspath(__file__))
RES  = os.path.join(HERE, "..", "result")
DIR  = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob(os.path.join(RES, "breakdown_*")))[-1]
CSV  = os.path.join(DIR, "breakdown.csv")

GRAPH    = os.environ.get("GRAPH", "PT")
# element toggles for multi-panel layouts (shared legend / shared axis labels)
NOLEGEND = os.environ.get("NOLEGEND") == "1" or os.environ.get("BARE") == "1"
NOXLABEL = os.environ.get("NOXLABEL") == "1"
NOYLABEL = os.environ.get("NOYLABEL") == "1"
NOTITLE  = os.environ.get("NOTITLE") == "1"
DPUS     = [64, 128, 256, 512, 1024]
PATTERNS = [("HOUSE5", "P1")]
ENGINES  = [("pimpam", "PimPam", ""), ("pimgrasp", "PIM-grasp", "\\\\")]

C_READ, C_TASK, C_SGEN = "#d6d6d6", "#204d78", "#7dbfe0"
C_SGX, C_MATCH, C_RSX  = "#b8773f", "#58a873", "#5a1a1d"
C_MX = "#c0392b"   # PIM-grasp fused sg gen+match+xfer

# optional: split PimPam's sg gen into DPU work / transfer / host regions (SGSPLIT=1)
# (label, color, [csv columns summed]), aggregated over the 3 modes
SGGEN_REGIONS = [
    ("sg:dpu",  "#3182bd", ["m0_exec", "m1_exec", "m2_exec"]),
    ("sg:xfer", "#e6550d", ["m0_xfer", "m1_xfer", "m2_xfer"]),
    ("sg:host", "#b2df8a", ["m0_host", "m1_host", "m2_host"]),
]
def load_sggen():
    fs = sorted(glob.glob(os.path.join(RES, "sggen_regions_*.csv")))
    if not fs: return None
    d = {}
    with open(fs[-1]) as f:
        for r in csv.DictReader(f):
            if r["cell"] != GRAPH: continue
            d[(r["pattern"], int(r["dpus"]))] = {lab: sum(_f(r.get(c)) for c in cols)
                                                 for lab, _, cols in SGGEN_REGIONS}
    return d or None

def _f(x):
    try: return float(x)
    except (TypeError, ValueError): return 0.0
SGGEN = load_sggen() if os.environ.get("SGSPLIT") == "1" else None

# CSV: (pattern,engine,dpus) -> dict of phase ms
csv_rows = {}
with open(CSV) as f:
    for r in csv.DictReader(f):
        if r["graph"] != GRAPH: continue
        csv_rows[(r["pattern"], r["engine"], int(r["dpus"]))] = r

def segments(eng, pat, d):
    """list of (value_seconds, colour). Bars sum to E2E."""
    r = csv_rows.get((pat, eng, d))
    if r is None: return None
    e2e  = _f(r["e2e_ms"]) / 1000.0
    comp = _f(r["comp_exposed_ms"]) / 1000.0
    xfer = _f(r["xfer_ms"]) / 1000.0
    ex   = _f(r["exec_ms"]) / 1000.0
    coll = _f(r["collect_ms"]) / 1000.0
    other= _f(r.get("other_ms")) / 1000.0
    if eng == "pimpam":
        readsort, task = _f(r.get("readsort_ms")) / 1000.0, _f(r.get("task_ms")) / 1000.0
        # sg gen absorbs host staging so the bar closes to E2E
        sgen = max(e2e - readsort - task - xfer - ex - coll, 0.0)
        segs = [(readsort, C_READ), (task, C_TASK)]
        if SGGEN and (pat, d) in SGGEN:                 # split sg gen into build regions
            reg = SGGEN[(pat, d)]; rtot = sum(reg.values()) or 1.0
            for lab, color, cols in SGGEN_REGIONS:
                segs.append((sgen * reg[lab] / rtot, color))
        else:
            segs.append((sgen, C_SGEN))
        segs += [(xfer, C_SGX), (ex, C_MATCH), (coll, C_RSX)]
        return segs
    # PIM-grasp: read+sort (prep) + fused sg gen+match+xfer
    prep = _f(r["prep_ms"]) / 1000.0
    fused = comp + xfer + ex + coll + other
    return [(prep, C_READ), (fused, C_MX)]

# legend grouped by engine: PimPam row (solid) + PIM-grasp row (solid + hatch),
# phase columns aligned across rows via blank placeholders
from matplotlib.ticker import MultipleLocator
matplotlib.rcParams["hatch.linewidth"] = 1.0
HATCH = "\\\\"
def sw(c, h, lab): return Patch(fc=c, ec="#222", lw=0.8, hatch=h, label=lab)
blank = Patch(fc="none", ec="none", label=" ")
_sg = ([sw(color, "", lab) for lab, color, cols in SGGEN_REGIONS]
       if SGGEN else [sw(C_SGEN, "", "sg gen")])
pim_handles = [sw(C_READ, "", "read+sort"), sw(C_TASK, "", "task assign")] + _sg + [
               sw(C_SGX, "", "sg xfer"), sw(C_MATCH, "", "match"), sw(C_RSX, "", "res xfer")]
NC = len(pim_handles)
# PIM-grasp: read+sort under col1, fused under the sg gen column (col 3)
grasp_handles = [sw(C_READ, HATCH, "read+sort"), blank,
                 sw(C_MX, HATCH, "sg gen+match+xfer")] + [blank]*(NC-3)

GROUP_W = 0.80
BAR_W   = GROUP_W / len(ENGINES) * 0.86

for pat, ptitle in PATTERNS:
    fig, ax = plt.subplots(figsize=(9.0, 5.1))
    # top reclaims the legend band when NOLEGEND; bottom leaves room for the x-label when present
    _top = 0.955 if NOLEGEND else 0.82
    _bot = 0.105 if NOXLABEL else 0.165
    _left = 0.135 if _f(csv_rows.get((pat,"pimpam",64),{}).get("e2e_ms",0)) >= 100000 else 0.088
    fig.subplots_adjust(left=_left, right=0.985, bottom=_bot, top=_top)
    xbase = list(range(len(DPUS)))
    for ei, (eng, elabel, hatch) in enumerate(ENGINES):
        off = (ei - (len(ENGINES) - 1) / 2.0) * (GROUP_W / len(ENGINES))
        for xi, d in enumerate(DPUS):
            seg = segments(eng, pat, d)
            if seg is None: continue
            bottom = 0.0
            for val, colour in seg:
                ax.bar(xbase[xi] + off, val, BAR_W, bottom=bottom,
                       color=colour, edgecolor="#222", linewidth=0.7, hatch=hatch, zorder=3)
                bottom += val
    ax.set_xticks(xbase)
    ax.set_xticklabels([str(d) for d in DPUS], fontsize=18)
    if not NOXLABEL:
        ax.set_xlabel("No. of DPUs", fontsize=20, labelpad=3)
    if not NOYLABEL:
        ax.set_ylabel("End-to-end time (sec)", fontsize=20)
        ax.yaxis.set_label_coords(-0.052, 0.5)
    from matplotlib.ticker import MaxNLocator
    ax.yaxis.set_major_locator(MaxNLocator(nbins=6, steps=[1,2,2.5,5,10]))
    ax.tick_params(axis="y", labelsize=18, pad=6)
    ax.tick_params(axis="x", pad=6)
    ax.margins(x=0.03)
    if not NOTITLE:
        ax.text(0.5, 0.965, GRAPH, transform=ax.transAxes,
                ha="center", va="top", fontsize=21)      # dataset name inside plot
    ax.grid(axis="y", color="#dddddd", linewidth=0.7, zorder=0)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    # two engine-labelled legend rows above the plot
    if not NOLEGEND:
        LX, Y1, Y2 = 0.145, 0.955, 0.885
        tkw = dict(fontsize=15.5, va="center", ha="right", fontweight="bold")
        lkw = dict(ncol=NC, loc="center left", frameon=False, fontsize=16.5,
                   columnspacing=0.32, handlelength=0.72, handletextpad=0.22)
        fig.text(LX-0.015, Y1, "PimPam", **tkw)
        l1 = fig.legend(handles=pim_handles,   bbox_to_anchor=(LX, Y1), **lkw); fig.add_artist(l1)
        fig.text(LX-0.015, Y2, "PIM-grasp", **tkw)
        fig.legend(handles=grasp_handles, bbox_to_anchor=(LX, Y2), **lkw)
    # HOUSE5 (P1) -> breakdown_dpu_<graph>; other patterns keep the pattern suffix
    base = f"breakdown_dpu_{GRAPH}" if pat == "HOUSE5" else f"breakdown_dpu_{GRAPH}_{pat}"
    out = os.path.join(HERE, "..", "figure", base)
    for ext in ("png", "pdf", "eps"):
        fig.savefig(f"{out}.{ext}", dpi=150)
    plt.close(fig)
    print("wrote", out + ".{png,pdf,eps}")
