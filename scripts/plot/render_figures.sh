#!/usr/bin/env bash
# render_figures.sh -- render the figures from the plot inputs in data/.
# Called by the plot phase of exp_all.sh.
#
# Each generator in code/ runs in a temporary directory of symlinks into data/, and the
# rendered images are collected into OUTDIR. No DPU hardware or UPMEM SDK is needed.
#
# Usage:   scripts/plot/render_figures.sh [OUTDIR]
#          OUTDIR defaults to figures_out/ (created if absent).
#
# Requires: python3 (+matplotlib, numpy) and gnuplot on PATH.
set -euo pipefail

BUNDLE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="$BUNDLE/data"
CODE="$BUNDLE/code"
OUT="${1:-$BUNDLE/figures_out}"

command -v python3 >/dev/null || { echo "ERROR: python3 not found" >&2; exit 1; }
command -v gnuplot >/dev/null || { echo "ERROR: gnuplot not found" >&2; exit 1; }
python3 -c "import matplotlib, numpy" 2>/dev/null || {
  echo "ERROR: python3 needs matplotlib and numpy" >&2; exit 1; }

mkdir -p "$OUT"

# ---- scope: render ONLY the experiments/graphs this invocation ran -----------
# exp_all.sh exports EXPERIMENTS (and ONLY_GRAPHS when set) so the plot step mirrors
# the run/parse scope: a PT-only or degcpu-only run emits figures for PT / degcpu
# ALONE, and never re-renders a figure for an experiment or graph that was not run
# this time from a stale input left in data/ by an earlier, broader run. Invoked
# standalone (no EXPERIMENTS in the environment), it defaults to every figure.
EXPERIMENTS="${EXPERIMENTS:-overall overlap tasklet_idle ondemand batching collect degcpu}"
GRAPHS_WANT="${ONLY_GRAPHS:-PT CJ PK LJ}"
has(){ case " $EXPERIMENTS " in *" $1 "*) return 0;; *) return 1;; esac; }
want_graph(){ case " $GRAPHS_WANT " in *" $1 "*) return 0;; *) return 1;; esac; }

# ---- throwaway staging tree mirroring the layout the generators expect -------
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/fig_repro.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
PA="$STAGE/pa"                              # stand-in for the bundle root
mkdir -p "$PA/code/gnuplot" "$PA/result/lb" \
         "$PA/result/breakdown_latest" "$PA/paper_figure_data" \
         "$PA/sections/figures" "$PA/figure" "$STAGE/figure"

ln(){ command ln -sf "$@"; }               # idempotent symlinks

# generator scripts
for f in "$CODE"/*.py "$CODE"/*.gp; do ln "$f" "$PA/code/$(basename "$f")"; done

# CSV/.dat the python scripts read from their own dir (HERE/)
ln "$DATA/collect.csv"            "$PA/code/collect.csv"
ln "$DATA/breakdown_fig1cj.csv"   "$PA/code/breakdown.csv"          # renamed to avoid clash
ln "$DATA/breakdown_overall.csv"  "$PA/code/breakdown_overall.csv"

# .dat the gnuplot scripts read from cwd (= code/)
for g in PT CJ LJ PK; do ln "$DATA/overall_p14_$g.dat" "$PA/code/overall_p14_$g.dat"; done
for g in PT LJ; do ln "$DATA/dpu_speedup_$g.dat" "$PA/code/dpu_speedup_$g.dat"; done

# result/ tree
ln "$DATA/expoverlap_full.csv"           "$PA/result/expoverlap_full.csv"
for f in "$DATA"/expbatch_*.csv; do ln "$f" "$PA/result/$(basename "$f")"; done
ln "$DATA/breakdown_dpusweep.csv"        "$PA/result/breakdown_latest/breakdown.csv"
ln "$DATA/degcpu_house5_cit-Patents.csv" "$PA/result/degcpu_house5_cit-Patents.csv"
ln "$DATA/degcpu_p_p6_cit-Patents.csv"   "$PA/result/degcpu_p_p6_cit-Patents.csv"
for f in "$DATA"/lb/*; do ln "$f" "$PA/result/lb/$(basename "$f")"; done

# paper_figure_data/ (on-demand per-DPU CSVs)
ln "$DATA/ondemand_p1" "$PA/paper_figure_data/ondemand_p1"
ln "$DATA/ondemand_p2" "$PA/paper_figure_data/ondemand_p2"

# ---- run every generator with the cwd / env / argv it expects ----------------
# Each generator is fault-tolerant: if its input data is absent (e.g. only a
# subset of experiments was run) it is skipped with a note instead of aborting
# the whole render. This keeps a partial run (or a trimmed EXPERIMENTS set)
# producing every figure it CAN, and still runs the collection step below.
echo ">> rendering (staging: $STAGE)"
rendered=0; skipped=0; found=0
STAMP="$STAGE/.genstamp"
# Collect ONLY the figures the just-run generator actually produced: non-empty images
# newer than the pre-run stamp. This excludes (1) the 0-byte stubs gnuplot leaves behind
# when a 'set output' plot fails on missing data, and (2) stale outputs from other gens.
collect_new(){
  while IFS= read -r -d '' img; do
    cp -f "$img" "$OUT/$(basename "$img")"; found=$((found + 1))
  done < <(find "$PA/figure" "$PA/sections/figures" "$STAGE/figure" "$PA/code" \
                -type f -newer "$STAMP" -size +0c \
                \( -name '*.png' -o -name '*.pdf' -o -name '*.eps' \) -print0 2>/dev/null)
}
gen(){ # gen "<label>" "<shell command>"
  : > "$STAMP"; sleep 0.01            # stamp just before the generator writes its outputs
  if ( eval "$2" ) >/dev/null 2>&1; then
    rendered=$((rendered + 1)); collect_new
  else
    echo "  [skip] $1 (inputs missing or generator error)"; skipped=$((skipped + 1))
  fi
}

# Each generator is guarded by has <experiment> (and want_graph <G> for per-graph
# figures) so the set of rendered figures is exactly this run's scope. overall and
# breakdown_overall take the wanted-graph list so they render only those panels.

# gnuplot generators (cwd = code/, write ../../figure -> $STAGE/figure)
has overall                    && gen "overall ($GRAPHS_WANT)" "cd '$PA/code' && gnuplot -e \"GRAPHS='$GRAPHS_WANT'\" overall_sub.gp"
has overall && want_graph PT && gen "dpu_speedup_PT"  "cd '$PA/code' && gnuplot -e \"GRAPH='PT'\" dpu_speedup.gp"
has overall && want_graph LJ && gen "dpu_speedup_LJ"  "cd '$PA/code' && gnuplot -e \"GRAPH='LJ'\" dpu_speedup.gp"

# python generators that resolve paths from the script dir (HERE/..)
has overlap      && gen "overlap_{PT,CJ,PK,LJ}" "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_overlap_subfigs.py"
has tasklet_idle && gen "tasklet_idle"          "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_tasklet_idle_p1p2.py"
has ondemand     && gen "ondemand_mean"         "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_ondemand_p1p2.py"
has collect      && gen "collect_compare"       "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_collect_mean.py"
has collect      && gen "collect_nbatch"        "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_collect_nbatch.py"
has overall && want_graph CJ && gen "breakdown (fig1cj)" "cd '$PA/code' && python3 plot_fig1_cj.py"
has overall      && gen "breakdown_overall"     "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_breakdown_overall.py"
has batching     && gen "batching_{nbatch,xfer}" "cd '$PA/code' && GRAPHS='$GRAPHS_WANT' python3 plot_batching_house5.py"
has overall && want_graph PT && gen "breakdown_dpu_PT" "cd '$PA/code' && GRAPH=PT python3 plot_breakdown_dpusweep.py"

# python generators that use cwd-relative paths (cwd = paper_artifacts stand-in)
has degcpu && want_graph PT && gen "deg_*_PT_cpu" "cd '$PA' && python3 code/plot_deg_pat_cpu.py cit-Patents PT"

# ---- outputs were already collected per-generator (collect_new) --------------
echo ">> generators: $rendered rendered, $skipped skipped"
echo ">> copied $found image files into: $OUT"
ls "$OUT" | sed 's/^/   /'
