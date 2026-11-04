#!/usr/bin/env python3
# Overall-style breakdown: one subplot per dataset (PT/PK/CJ/LJ), each with three horizontal
# bars (PimPam / OFIS-PimPam / PIM-grasp) stacked into phases that sum to the measured E2E:
#     prep | subgraph gen | subgraph xfer | exec | result xfer | other
# Reads breakdown_overall.csv; 'other' is derived from E2E if the column is absent.
#   python3 code/plot_breakdown_overall.py [PATTERN ...]   (default: HOUSE5)
import os, sys, csv, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["font.sans-serif"] = ["Arial", "Liberation Sans", "DejaVu Sans"]
matplotlib.rcParams["pdf.fonttype"] = 42
matplotlib.rcParams["ps.fonttype"] = 42   # Type42 (TrueType), avoids Type3

HERE = os.path.dirname(os.path.abspath(__file__))
RES  = os.path.join(HERE, "..", "result")
OUT  = os.path.join(HERE, "breakdown_overall")
# patterns to show. DEFAULT = P1 (HOUSE5) only -> the paper's overall breakdown figure: one
# pattern, a 2x2 over the four datasets. Pass patterns as args (e.g. "HOUSE5 K4_TAIL5") to get
# the pattern-per-row grid instead (heavy vs light comparison).
_args = " ".join(sys.argv[1:]).replace(",", " ").split()
PATTERNS = _args if _args else ["HOUSE5"]

# datasets to show (canonical order); GRAPHS env restricts to this run's scope so a
# PT-only run renders a PT-only figure instead of one with empty CJ/PK/LJ panels.
_CANON = ["PT", "PK", "CJ", "LJ"]
_want = os.environ.get("GRAPHS", "").replace(",", " ").split()
DATASETS = [d for d in _CANON if d in _want] if _want else _CANON
ENGINES  = [("pimpam", "PimPam"), ("ofis", "OFIS-PimPam"), ("pimgrasp", "PIM-grasp")]

# phase key -> (label, colour); order == stacking (pipeline) order
PHASES = [
    ("prep",     "read+sort", "#d6d6d6"),
    ("sg_gen",   "sg_gen",    "#7dbfe0"),
    ("sg_xfer",  "sg_xfer",   "#96551f"),
    ("exec",     "match",     "#58a873"),
    ("res_xfer", "res_xfer",  "#5a1a1d"),
    ("other",    "other",     "#7a63ab"),
]
# engine shown by fill: PimPam solid; the two OFIS-based engines both hatched (opposite
# diagonals) so they stay distinguishable in B&W
ENGINE_HATCH = {"pimpam": "", "ofis": "////", "pimgrasp": "\\\\"}
matplotlib.rcParams["hatch.linewidth"] = 1.2

# PimPam keeps the detailed phases; the OFIS-based engines fuse sg xfer + match + res xfer
# + other into one "match+xfer" phase (its dispatch loop interleaves them)
C_READ, C_SGEN, C_SGX = "#d6d6d6", "#7dbfe0", "#b8773f"
C_MATCH, C_RSX, C_MX, C_SMX  = "#58a873", "#5a1a1d", "#bea7ef", "#cc5555"
C_TASK = "#204d78"   # task assign (PimPam only)

# breakdown_overall.csv carries the FULL DPU sweep (64..1024, shared with the breakdown-vs-DPU
# figure). The overall-style breakdown must show ONE headline DPU count -- the same config the
# overall figure uses (NR_DPUS=1024) -- not the first row in the file (which is the 64-DPU point
# and ~8x slower). Pick the largest DPU count present by default (= 1024 in the standard full
# run); override with BD_DPUS=256 to pin a different point.
def _target_dpus():
    env = os.environ.get("BD_DPUS", "").strip()
    if env:
        return env
    path = os.path.join(HERE, "breakdown_overall.csv")
    mx = None
    if os.path.exists(path):
        with open(path) as f:
            for r in csv.DictReader(f):
                try:
                    d = int((r.get("dpus") or "").strip())
                except ValueError:
                    continue
                if mx is None or d > mx:
                    mx = d
    return str(mx) if mx is not None else ""
TARGET_DPUS = _target_dpus()

def load_row(ds, eng, pattern):
    """Phases (sec) for (dataset, engine, pattern) at the headline DPU count TARGET_DPUS, or None."""
    path = os.path.join(HERE, "breakdown_overall.csv")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for r in csv.DictReader(f):
            if (r["engine"] == eng and r["graph"] == ds and r["pattern"] == pattern
                    and (not TARGET_DPUS or (r.get("dpus") or "").strip() == TARGET_DPUS)):
                g = lambda k: float(r.get(k) or 0.0)
                e2e   = g("e2e_ms")
                prep  = g("prep_ms")
                sgen  = g("comp_exposed_ms")
                sgx   = g("xfer_ms")
                ex    = g("exec_ms")
                rsx   = g("collect_ms")
                other = float(r["other_ms"]) if r.get("other_ms") not in (None, "") \
                        else max(e2e - (prep + sgen + sgx + ex + rsx), 0.0)
                return {k: v / 1000.0 for k, v in
                        dict(prep=prep, sg_gen=sgen, sg_xfer=sgx, exec=ex,
                             res_xfer=rsx, other=other, e2e=e2e,
                             readsort=g("readsort_ms"), task=g("task_ms")).items()}
    return None

# layout: single pattern -> 2x2 over datasets; multiple patterns -> rows(patterns) x cols(datasets)
if len(PATTERNS) == 1:
    NR, NC = 2, 2
    ORDER2x2 = ["PT", "CJ", "PK", "LJ"]   # row-major -> top: PT|CJ, bottom: PK|LJ
    # scope to the datasets actually run (DATASETS already respects the GRAPHS env): a full run
    # keeps the paper's four-panel 2x2; a graph-restricted run (e.g. PT-only) drops the panels
    # it has no data for instead of drawing empty "no CJ/PK/LJ" quadrants.
    cells = [(PATTERNS[0], ds) for ds in ORDER2x2 if ds in DATASETS]
    fig, axf = plt.subplots(NR, NC, figsize=(10.4, 5.2), squeeze=False)
    axlist = [a for row in axf for a in row]
    for _a in axlist[len(cells):]:        # hide unused quadrants in a reduced run
        _a.axis("off")
else:
    NR, NC = len(PATTERNS), len(DATASETS)
    cells = [(p, ds) for p in PATTERNS for ds in DATASETS]
    fig, axf = plt.subplots(NR, NC, figsize=(3.0 * NC, 1.9 * NR + 0.6), squeeze=False)
    axlist = [a for row in axf for a in row]
def segments(ekey, row):
    if ekey == "pimpam":
        # read+sort and task assign come from their own CSV columns; sg gen is the residual so the bar
        # sums to the real makespan
        sgen = max(row["e2e"] - row["readsort"] - row["task"]
                   - row["sg_xfer"] - row["exec"] - row["res_xfer"], 0.0)
        return [(row["readsort"], C_READ), (row["task"], C_TASK), (sgen, C_SGEN),
                (row["sg_xfer"], C_SGX), (row["exec"], C_MATCH), (row["res_xfer"], C_RSX)]
    mx = row["sg_xfer"] + row["exec"] + row["res_xfer"] + row["other"]
    if ekey == "ofis":
        return [(row["prep"], C_READ), (row["sg_gen"], C_SGEN), (mx, C_MX)]
    else:
        # PIM-grasp: sg_gen is fully pipelined, so fold it into the fused segment
        return [(row["prep"], C_READ), (row["sg_gen"] + mx, C_SMX)]

YPOS = [2, 1, 0]                       # PimPam top, PIM-grasp bottom
drew_any = False                       # any (engine,graph,pattern) row actually plotted?
for idx, (pattern, ds) in enumerate(cells):
        ax = axlist[idx]
        ri, ci = idx // NC, idx % NC
        any_row = False
        cell_max = 0.0
        for (ekey, _elabel), y in zip(ENGINES, YPOS):
            row = load_row(ds, ekey, pattern)
            if row is None:
                continue
            any_row = True; drew_any = True
            left = 0.0
            for v, c in segments(ekey, row):
                ax.barh(y, v, height=0.62, left=left, color=c,
                        edgecolor="#222", linewidth=0.8, hatch=ENGINE_HATCH[ekey])
                left += v
            cell_max = max(cell_max, left)
        single = (len(PATTERNS) == 1)
        ax.set_yticks([])   # engines identified by hatch in the legend, no left labels
        if single or ri == 0:
            ax.set_title(ds, fontsize=19, pad=2)
        if (not single) and ci == 0:
            ax.set_ylabel(pattern, fontsize=19)
        if idx + NC >= len(cells):                  # x-label on the bottom panel of each column
            ax.set_xlabel("Execution time (sec)", fontsize=19)
        ax.tick_params(axis="x", labelsize=17)
        ax.margins(y=0.20)
        # pin the x-range to the longest bar so every panel's bars reach its right edge
        if cell_max > 0:
            ax.set_xlim(0, cell_max * 1.01)
            ax.set_autoscalex_on(False)
        for s in ("top", "right", "left"):
            ax.spines[s].set_visible(False)
        if not any_row:
            ax.text(0.5, 0.5, f"no {ds}/{pattern}", ha="center", va="center",
                    transform=ax.transAxes, fontsize=10, color="#999")

# legend: one row per engine. PimPam (solid) shows its phases; the two OFIS-based engines
# share the fused phase set with opposite diagonal hatches. Columns aligned across rows.
def sw(c, hatch, lab):
    return Patch(fc=c, ec="#222", linewidth=0.8, hatch=hatch, label=lab)
blank = Patch(fc="none", ec="none", label="")
pim_handles = [sw(C_READ, "", "read+sort"), sw(C_TASK, "", "task_assign"),
               sw(C_SGEN, "", "sg_gen"), sw(C_SGX, "", "sg_xfer"),
               sw(C_MATCH, "", "match"), sw(C_RSX, "", "res_xfer")]
def ofis_row(h):
    # blanks keep read+sort (col 1), sg_gen (col 3), match+xfer (col 5) aligned with PimPam
    return [sw(C_READ, h, "read+sort"), blank, sw(C_SGEN, h, "sg_gen"),
            blank, sw(C_MX, h, "match+xfer"), blank]
def grasp(h):
    # PIM-grasp folds sg_gen into the fused segment; combined bar sits in col 3 to line up
    return [sw(C_READ, h, "read+sort"), blank, sw(C_SMX, h, "sg_gen+match+xfer"),
            blank, blank, blank]
LABEL_X, LX = 0.010, 0.190
Y_PP, Y_OF, Y_GW = 1.030, 0.975, 0.920   # three engine rows
lblkw = dict(fontsize=18, va="center", ha="left", fontweight="bold")
legkw = dict(ncol=6, fontsize=18, frameon=False, loc="center left",
             handlelength=1.0, columnspacing=0.6, handletextpad=0.3)
fig.text(LABEL_X, Y_PP, "PimPam", **lblkw)
fig.legend(handles=pim_handles, bbox_to_anchor=(LX, Y_PP), **legkw)
fig.text(LABEL_X, Y_OF, "OFIS-PimPam", **lblkw)
fig.legend(handles=ofis_row(ENGINE_HATCH["ofis"]), bbox_to_anchor=(LX, Y_OF), **legkw)
fig.text(LABEL_X, Y_GW, "PIM-grasp", **lblkw)
fig.legend(handles=grasp(ENGINE_HATCH["pimgrasp"]), bbox_to_anchor=(LX, Y_GW), **legkw)
plt.tight_layout(rect=(0, 0, 1, 0.895))   # room for three legend rows
# widen all 4 panels uniformly so the right column reaches the legend's right end
if len(PATTERNS) == 1:
    L, RIGHT, GAP = 0.022, 1.062, 0.028
    W = (RIGHT - L - GAP) / 2.0
    for _i, _ax in enumerate(axlist):
        _p = _ax.get_position()
        _x0 = L if (_i % NC) == 0 else (L + W + GAP)
        _ax.set_position([_x0, _p.y0, W, _p.height])
# No data for any panel (e.g. breakdown_overall.csv absent / wrong pattern): do not emit an
# empty "no X" figure -- exit nonzero so render_figures.sh records it as skipped.
if not drew_any:
    sys.stderr.write("plot_breakdown_overall: no rows for %s -- nothing to plot\n"
                     % "+".join(PATTERNS))
    sys.exit(1)
for ext in ("png", "pdf", "eps"):
    fig.savefig(f"{OUT}.{ext}", dpi=300, bbox_inches="tight")
print(f"wrote {OUT}.{{png,pdf,eps}}  (patterns={'+'.join(PATTERNS)}, dpus={TARGET_DPUS or 'any'})")
