#!/bin/bash
# Intra-DPU tasklet IDLE time: for each work-unit (cooperative heavy root / light phase) the idle
# is Σ_j (max_tasklet - tasklet_j) = NR_TASKLETS*max - Σ, accumulated per DPU (kernel -DTASKLET_IDLE
# puts it in cycle_ct, so the usual per-DPU dump now holds idle cycles).
# Compares on-demand intra-DPU tasklet stealing OFF (static assignment) vs ON (-D<PAT>_DYNAMIC).
#   CELLS="CJ LJ" PATTERN=HOUSE5 bash scripts/run/tasklet_idle.sh
# Output: result/lb/idle_<PAT>_<G>_{off,on}.txt  (DPU/tasklet cycle dump = idle) + a summary.
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME=${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}
export PATH=$UPMEM_HOME/bin:$PATH
COMMON="NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16"
CELLS=${CELLS:-"PT CJ PK LJ"}; PATTERN=${PATTERN:-HOUSE5}
# PIM-grasp (BARRIER hybrid, GW_BD -- same engine as exp_ondemand_perdpu.sh) with -DPERF so each
# batch's per-tasklet cycles are available. -DTASKLET_IDLE makes cycle_ct hold the per-unit idle
# (16*max-Σ): heavy = per-root (batch size 1), light = per-batch phase; g_dpu_cycle_ct accumulates
# these across all batches per DPU -> the DPUK dump is the per-DPU tasklet-idle. OFF vs ON = the
# intra-DPU on-demand tasklet stealing (-D<PAT>_DYNAMIC) toggled on that same batched engine.
BASE="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 \
-DOFIS_COLLECT_ON_FINAL_RETIRE -DOFIS_SCALAR_ANS -DPERF -DOFIS_LEAN -DTASKLET_IDLE"
DYN="-D${PATTERN}_DYNAMIC"   # intra-DPU on-demand tasklet stealing ON
RUN="sudo stdbuf -oL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"
CLK=350000000.0

echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1
( while :; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & KA=$!
trap 'kill "$KA" 2>/dev/null' EXIT INT TERM
mkdir -p result/lb

# sum of per-DPU idle cycles from a DPUK dump (field 3 = idle cycles) -> seconds
idlesum(){ awk '/^DPUK /{s+=$3} END{printf "%.6f", s/'"$CLK"'}' "$1"; }

run_variant(){ local G=$1 tag=$2 defs=$3
  make clean >/dev/null 2>&1
  make -j16 all GRAPH=$G PATTERN=$PATTERN $COMMON EXTRA_DEFS="$defs" >${TMPDIR:-/tmp}/pimgrasp.$$.idle.build 2>&1 \
    || { echo "  BUILD FAIL $G/$tag: $(tail -3 ${TMPDIR:-/tmp}/pimgrasp.$$.idle.build)"; return 1; }
  local dst="result/lb/idle_${PATTERN}_${G}_${tag}.dpuk"
  $RUN 2>&1 | tee ${TMPDIR:-/tmp}/pimgrasp.$$.idle_${G}_${tag}.log | grep -a '^DPUK ' > "$dst"
  local n; n=$(wc -l < "$dst")
  [ "$n" -ge 1 ] || { echo "  WARNING: no DPU kernel-time samples (check PERF build)"; return 1; }
  local ans; ans=$(grep -aoE 'DPU ans: [0-9]+' ${TMPDIR:-/tmp}/pimgrasp.$$.idle_${G}_${tag}.log | grep -oE '[0-9]+' | tail -1)
  printf "  [%s] idle=%ss rows=%s ans=%s -> %s\n" "$tag" "$(idlesum "$dst")" "$n" "${ans:-?}" "$dst"
}

echo "graph  idle_OFF(s)  idle_ON(s)  reduction%"
for G in $CELLS; do
  echo "================= $G / $PATTERN ================="
  run_variant "$G" off "$BASE"        || continue
  run_variant "$G" on  "$BASE $DYN"   || continue
  off=$(idlesum "result/lb/idle_${PATTERN}_${G}_off.dpuk")
  on=$(idlesum  "result/lb/idle_${PATTERN}_${G}_on.dpuk")
  awk -v g="$G" -v o="$off" -v n="$on" 'BEGIN{printf "%-6s %11.2f %11.2f %9.1f%%\n", g, o, n, (o>0?100*(o-n)/o:0)}'
done
echo "==== DONE ===="
