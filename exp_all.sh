#!/usr/bin/env bash
# exp_all.sh -- end-to-end "run to reproduce" orchestrator for the PIM-grasp paper artifacts.
# Mirrors the UPMEM OFIS exp_all.sh: build -> run every experiment on the UPMEM HW -> parse
# the raw logs into plot inputs -> render the figures.
#
#   ./exp_all.sh [num_iter]           # num_iter = repetitions per data point (default 1)
#   PHASES="parse plot" ./exp_all.sh  # re-derive + render from an EXISTING result/ (no HW)
#   EXPERIMENTS="overall overlap" ./exp_all.sh 3   # a subset, 3 reps
#
# Phases (override with PHASES="..."):  setup run parse plot
# Experiments (override with EXPERIMENTS="..."):
#   DEFAULT (the figures in the paper):
#     overall overlap tasklet_idle ondemand batching collect degcpu
#     (overall -> one run behind Figures 1, 8, 9, 10, 11 and the "with overlap" arm of Figure 15;
#      overlap therefore also runs overall.)
#     (degcpu -> deg_HOUSE5_PT_cpu + deg_P_P6_PT_cpu, Fig. "degree vs CPU time"; CPU-only, PT.)
#
# The experiment -> paper figure mapping is in README.md ("Experiments and Paper Figures").
#
# The RUN phase needs the UPMEM HW + SDK (UPMEM_HOME) + datasets (see download_dataset.sh) and
# enables kernel.numa_balancing for its duration. The PARSE and PLOT phases are CPU-only and need
# no SDK. Raw logs land in result/, plot inputs in data/, figures in figures_out/.
set -u

BUNDLE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$BUNDLE/src"
RESULT="$BUNDLE/result"
DATASET="$(realpath -m "${DATA_DIR:-$BUNDLE/dataset}")"   # absolute: src/data/bin links to it
DATA="$BUNDLE/data"
RUNDIR="$BUNDLE/scripts/run"
PARSEDIR="$BUNDLE/scripts/parse"

NUM_ITER="${1:-${REPS:-1}}"
export REPS="$NUM_ITER"
PHASES="${PHASES:-setup run parse plot}"
EXPERIMENTS="${EXPERIMENTS:-overall overlap tasklet_idle ondemand batching collect degcpu}"

mkdir -p "$RESULT" "$DATA" "$DATA/lb" "$DATA/ondemand_p1" "$DATA/ondemand_p2"

has(){ case " $EXPERIMENTS " in *" $1 "*) return 0;; *) return 1;; esac; }
# Figure 15's "with overlap" arm comes from the overall run.
if has overlap && ! has overall; then EXPERIMENTS="overall $EXPERIMENTS"; fi
phase(){ case " $PHASES " in *" $1 "*) return 0;; *) return 1;; esac; }
newest(){ ls -t "$@" 2>/dev/null | head -1; }
say(){ echo; echo "#################### $* ####################"; }

# The paper's numbers were measured with automatic NUMA balancing on, and DPU-run time depends
# on it. Turn it on for the RUN phase if needed, and put the previous value back on exit.
NUMA_KNOB=/proc/sys/kernel/numa_balancing
NUMA_RESTORE=""
restore_numa_balancing(){
  [ -n "$NUMA_RESTORE" ] || return 0
  if echo "$NUMA_RESTORE" | sudo -n tee "$NUMA_KNOB" >/dev/null 2>&1; then
    echo "  kernel.numa_balancing restored to $NUMA_RESTORE"
  else
    echo "  WARNING: could not restore kernel.numa_balancing to $NUMA_RESTORE (sudo expired); set it manually."
  fi
}
require_numa_balancing(){
  [ -r "$NUMA_KNOB" ] || { echo "  WARNING: $NUMA_KNOB not found; cannot enable NUMA balancing."; return 0; }
  local cur; cur="$(cat "$NUMA_KNOB")"
  [ "$cur" = 1 ] && return 0
  echo "  kernel.numa_balancing=$cur; setting it to 1 for this run (the paper's configuration)."
  sudo -v && echo 1 | sudo tee "$NUMA_KNOB" >/dev/null \
    || { echo "ERROR: could not enable kernel.numa_balancing (sudo needed)." >&2; exit 1; }
  NUMA_RESTORE="$cur"
  trap restore_numa_balancing EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
}

############################## setup ##############################
if phase setup; then
  say "SETUP: bundle-relative symlinks (datasets, result)"
  mkdir -p "$SRC/data"
  # host reads ./data/bin/<DATA_NAME>.bin and writes ./result/ relative to cwd=src/.
  # src/data/bin and src/result are orchestrator-owned mount points. If either already exists
  # as a REAL dir (e.g. a stale src/result left by a prior in-tree build), `ln -sfn T link`
  # nests the link INSIDE it (src/result/result -> T) instead of replacing it -- the host then
  # writes under the real src/result and the parse step, which reads $RESULT, misses it. So
  # drop any non-symlink node first; an existing symlink is just retargeted by ln -sfn.
  for mp in "$SRC/data/bin" "$SRC/result"; do [ -L "$mp" ] || rm -rf "$mp"; done
  ln -sfn "$DATASET" "$SRC/data/bin"
  ln -sfn "$RESULT"  "$SRC/result"
  echo "  src/data/bin -> $DATASET"
  echo "  src/result   -> $RESULT"
  if [ ! -d "$DATASET" ] || [ -z "$(ls -A "$DATASET" 2>/dev/null)" ]; then
    echo "  WARNING: dataset dir is empty -- run ./download_dataset.sh before the RUN phase."
  fi
fi

############################## run ##############################
if phase run; then
  UPMEM_HOME="${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}"
  export UPMEM_HOME
  export PATH="$UPMEM_HOME/bin:$PATH"
  command -v dpu-upmem-dpurte-clang >/dev/null 2>&1 || {
    echo "ERROR: UPMEM SDK not on PATH (UPMEM_HOME=$UPMEM_HOME). Install it, then re-run." >&2; exit 1; }
  require_numa_balancing

  # ONLY_GRAPHS restricts every experiment to the listed graphs (e.g. ONLY_GRAPHS=PT
  # for a fast single-graph smoke test). Unset -> each experiment's full default set.
  # One run for Figures 1, 8, 9, 10, 11 and the "with overlap" arm of Figure 15, so that a point
  # shared by these figures has one value: P1-P5 x graphs at 1,024 DPUs (3 engines), plus the DPU
  # sweep of Figures 10 and 11 on PT and LJ (the two engines these figures compare).
  if has overall; then
    say "RUN overall (run_breakdown.sh) -- feeds Figures 1, 8, 9, 10, 11 and overlap"
    BD_LOGDIR="$RESULT/breakdown_$(date +%Y%m%d-%H%M%S)"
    ALL_PATTERNS="HOUSE5 HOUSE5_X K4_TAIL5 P_P6 HOUSE5_Y"
    CELLS="${ONLY_GRAPHS:-PT CJ PK LJ}" PATTERNS="$ALL_PATTERNS" DPUS="1024" \
      LOGDIR="$BD_LOGDIR" bash "$RUNDIR/run_breakdown.sh"
    # SWEEP_GRAPHS="PT" SWEEP_PATTERNS="HOUSE5" keeps only the part of the sweep behind Figure 11.
    SWEEP=""; for g in ${SWEEP_GRAPHS:-PT LJ}; do case " ${ONLY_GRAPHS:-PT LJ} " in *" $g "*) SWEEP="$SWEEP $g";; esac; done
    if [ -n "$SWEEP" ]; then
      CELLS="$SWEEP" PATTERNS="${SWEEP_PATTERNS:-$ALL_PATTERNS}" DPUS="64 128 256 512" ENGINES="pimpam pimgrasp" APPEND_CSV=1 \
        LOGDIR="$BD_LOGDIR" bash "$RUNDIR/run_breakdown.sh"
    fi
  fi

  has overlap       && { say "RUN overlap (exp_overlap_wo.sh = w/o arm; run_breakdown supplies w/ arm)";
    CELLS="${ONLY_GRAPHS:-PT PK CJ LJ}" PATTERNS="HOUSE5 HOUSE5_X K4_TAIL5 P_P6 HOUSE5_Y" bash "$RUNDIR/exp_overlap_wo.sh"; }

  has tasklet_idle  && { say "RUN tasklet_idle (P1=HOUSE5, P2=HOUSE5_X)";
    for P in HOUSE5 HOUSE5_X; do CELLS="${ONLY_GRAPHS:-PT CJ PK LJ}" PATTERN="$P" bash "$RUNDIR/tasklet_idle.sh"; done; }

  has ondemand      && { say "RUN ondemand per-DPU (P1=HOUSE5, P2=HOUSE5_X)";
    for P in HOUSE5 HOUSE5_X; do CELLS="${ONLY_GRAPHS:-PT PK CJ LJ}" PATTERN="$P" bash "$RUNDIR/exp_ondemand_perdpu.sh"; done; }

  has batching      && { say "RUN batching (exp_batching_gran.sh -> expbatch CSV)";
    CELLS="${ONLY_GRAPHS:-PT PK CJ LJ}" PATTERNS="HOUSE5" bash "$RUNDIR/exp_batching_gran.sh"; }

  has collect       && { say "RUN collect compare (per-batch vs final-retire)";
    CELLS="${ONLY_GRAPHS:-PT PK CJ LJ}" PATTERN="HOUSE5" bash "$RUNDIR/exp_collect_compare.sh"; }

  # Figure 3 is PT-only; skip it in a run restricted to other graphs.
  if has degcpu; then case " ${ONLY_GRAPHS:-PT} " in *" PT "*)
    say "RUN degree-vs-CPU-time dump (PT; CPU only, no DPU)"
    CELLS="PT" PATTERNS="HOUSE5 P_P6" bash "$RUNDIR/run_degcpu.sh";;
  esac; fi
fi

############################## parse / normalize ##############################
if phase parse; then
  say "PARSE: raw result/ -> plot inputs in data/"

  # overall: newest result/breakdown_*/breakdown.csv -> the inputs of Figures 1, 8, 9, 10, 11
  if has overall; then
    bd="$(newest "$RESULT"/breakdown_*/breakdown.csv)"
    if [ -n "$bd" ]; then python3 "$PARSEDIR/parse_breakdown.py" "$bd" || echo "  (overall parse incomplete: see the errors above)"
    else echo "  (breakdown csv not found)"; fi
  fi

  # overlap: merge w/o (expoverlapwo_*) + w/ (breakdown pimgrasp) -> data/expoverlap_full.csv
  if has overlap; then B_CSV="$DATA/breakdown_overall.csv" python3 "$PARSEDIR/merge_overlap_full.py" || echo "  (overlap merge skipped: inputs missing)"; fi

  # tasklet_idle: copy per-DPU dumps result/lb/* -> data/lb/
  if has tasklet_idle; then
    if ls "$RESULT"/lb/* >/dev/null 2>&1; then cp -f "$RESULT"/lb/* "$DATA/lb/" 2>/dev/null; echo "  lb dumps -> data/lb/"; fi
  fi

  # ondemand: normalize newest result/perdpu_<CELL>_<PAT>_<t>_<stamp>.csv -> data/ondemand_p{1,2}/perdpu_<CELL>_<t>.csv
  if has ondemand; then
    for CELL in PT PK CJ LJ; do for t in off on; do
      s1="$(newest "$RESULT"/perdpu_${CELL}_HOUSE5_${t}_*.csv)";   [ -n "$s1" ] && cp -f "$s1" "$DATA/ondemand_p1/perdpu_${CELL}_${t}.csv"
      s2="$(newest "$RESULT"/perdpu_${CELL}_HOUSE5_X_${t}_*.csv)"; [ -n "$s2" ] && cp -f "$s2" "$DATA/ondemand_p2/perdpu_${CELL}_${t}.csv"
    done; done
    echo "  ondemand per-DPU CSVs -> data/ondemand_p{1,2}/"
  fi

  # batching: newest result/expbatch_*.csv -> data/
  if has batching; then eb="$(newest "$RESULT"/expbatch_*.csv)"; [ -n "$eb" ] && { cp -f "$eb" "$DATA/$(basename "$eb")"; echo "  batching: $(basename "$eb") -> data/"; }; fi

  # collect: newest result/collectcmp_HOUSE5_*.csv -> data/collect.csv
  if has collect; then cc="$(newest "$RESULT"/collectcmp_HOUSE5_*.csv)"; [ -n "$cc" ] && { cp -f "$cc" "$DATA/collect.csv"; echo "  collect: $(basename "$cc") -> data/collect.csv"; }; fi

  # degcpu: result/degcpu_*_cit-Patents.csv -> data/
  if has degcpu; then for f in "$RESULT"/degcpu_*_cit-Patents.csv; do [ -e "$f" ] && { cp -f "$f" "$DATA/$(basename "$f")"; echo "  degcpu: $(basename "$f") -> data/"; }; done; fi
fi

############################## plot ##############################
if phase plot; then
  say "PLOT: render this run's figures (render_figures.sh)"
  # figures_out holds exactly THIS run's figures: wipe it first so a narrower re-run
  # (e.g. ONLY_GRAPHS=PT, or a subset EXPERIMENTS) never leaves a previous, broader
  # run's figures behind. render_figures.sh scopes its generators to EXPERIMENTS
  # (and ONLY_GRAPHS when set), so stale inputs in data/ are not re-rendered.
  rm -rf "$BUNDLE/figures_out"; mkdir -p "$BUNDLE/figures_out"
  export EXPERIMENTS
  [ -n "${ONLY_GRAPHS+x}" ] && export ONLY_GRAPHS
  bash "$BUNDLE/scripts/plot/render_figures.sh" "$BUNDLE/figures_out"
fi

say "DONE"
echo "raw logs:    $RESULT"
echo "plot inputs: $DATA"
echo "figures:     $BUNDLE/figures_out"
