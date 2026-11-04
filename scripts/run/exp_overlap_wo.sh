#!/bin/bash
# Overlap figure, W/O-OVERLAP arm ONLY (BARRIER basis, to match the master breakdown run).
# The w/ overlap bar is REUSED from the master run_breakdown.sh PIM-grasp E2E (barrier);
# here we measure only the removed arm = bulk prebuild, no producer pipeline:
#   GW_BD (barrier hybrid, same as breakdown) + OFIS_PREBUILD_ALL + OFIS_NO_COMPACT_PIPELINE.
# All host-side optimizations (batching, granularity, on-demand, final-retire collect) are
# identical to the master; only subgraph BUILD scheduling differs (all upfront vs pipelined).
#   usage: CELLS="PT PK CJ LJ" PATTERNS="HOUSE5" REPS=1 bash exp_overlap_wo.sh
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME=${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}
export PATH=$UPMEM_HOME/bin:$PATH
CELLS=${CELLS:-"PT PK CJ LJ"}; PATTERNS=${PATTERNS:-"HOUSE5"}; REPS=${REPS:-1}
RUN="sudo stdbuf -oL -eL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"

# BARRIER hybrid (NO early-promote) -- verbatim GW_BD from run_breakdown.sh, + prebuild.
GW_BD="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 -DHOUSE5_MAX_BATCH_ROOTS=2048 \
-DOFIS_COLLECT_ON_FINAL_RETIRE -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
PRE="-DOFIS_PREBUILD_ALL -DOFIS_NO_COMPACT_PIPELINE"   # w/o overlap = bulk prebuild upfront

echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1; mkdir -p result
( while :; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & KA=$!
trap 'kill "$KA" 2>/dev/null' EXIT INT TERM
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="result/expoverlapwo_$STAMP.log"; exec > >(tee "$LOG") 2>&1
echo "==> $LOG  REPS=$REPS  CELLS=$CELLS  PATTERNS=$PATTERNS  (BARRIER + prebuild; w/ overlap reused from master)"

build(){ make clean >/dev/null 2>&1
  make -j16 all GRAPH=$1 PATTERN=$2 NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16 \
       EXTRA_DEFS="$3" >${TMPDIR:-/tmp}/pimgrasp.$$.ow.build 2>&1 \
       || { echo "  BUILD FAIL: $(tail -3 ${TMPDIR:-/tmp}/pimgrasp.$$.ow.build)"; return 1; }; }

run1(){ local tmp=${TMPDIR:-/tmp}/pimgrasp.$$.ow.out; $RUN 2>&1 | tee "$tmp" >/dev/null
  local e pb ans
  e=$(grep -aoE 'End-to-end Time \(ms\): [0-9.]+' "$tmp" | grep -oE '[0-9.]+' | tail -1)
  pb=$(grep -aF '[OFIS] bulk prebuild' "$tmp" | grep -oP 'resident,\s*\K[0-9.]+' | tail -1)
  ans=$(grep -aoE 'DPU ans: [0-9]+' "$tmp" | grep -oE '[0-9]+' | tail -1)
  echo "${e:-FAIL} ${pb:-0} ${ans:-?}"; }

best(){ local defs="$1" G="$2" P="$3" bE="" line e; local BP BA
  build "$G" "$P" "$defs" || { echo "FAIL 0 ?"; return; }
  for r in $(seq 1 $REPS); do line=$(run1); e=$(echo "$line"|cut -d' ' -f1)
    if [ -z "$bE" ] || awk -v a="$bE" -v x="$e" 'BEGIN{exit !(x<a)}'; then bE="$e"; read _ BP BA <<<"$line"; fi
  done; echo "${bE:-FAIL} ${BP:-0} ${BA:-?}"; }

declare -A R
for P in $PATTERNS; do for G in $CELLS; do
  echo; echo "########################  $G / $P  (w/o overlap = barrier + prebuild)  ########################"
  DYN="-D${P}_DYNAMIC -D${P}_MAX_BATCH_ROOTS=2048"
  R["$G/$P"]="$(best "$GW_BD $DYN $PRE" "$G" "$P")"
  echo "  e2e prebuild ans = ${R[$G/$P]}"
done; done

CSV="result/expoverlapwo_$STAMP.csv"
echo "cell,pattern,e2e_wo_ms,total_build_ms,ans" > "$CSV"
echo; echo "==============  W/O OVERLAP (barrier + bulk prebuild), ms  =============="
printf "%-11s %13s %13s   %s\n" "cell/pat" "E2E_wo" "total_build" "ans"
for P in $PATTERNS; do for G in $CELLS; do
  read e pb a <<<"${R[$G/$P]:-? 0 ?}"
  printf "%-11s %13s %13s   %s\n" "$G/$P" "$e" "$pb" "$a"
  echo "$G,$P,$e,$pb,$a" >> "$CSV"
done; done
echo
echo "CSV: $CSV"
