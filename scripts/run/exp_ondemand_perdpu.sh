#!/bin/bash
# Per-DPU execution-time spread for the intra-DPU on-demand study, ONE workload.
# DPU_TIME_PROFILE dumps each DPU's host-observed kernel wall (g_dpu_kernel_ms =
# sum(finish-launch)) over ALL THREE tiers (DPU + IG + RANK), i.e. each DPU's total
# busy time across the whole run. We run the SAME workload twice on the BARRIER hybrid:
#   OFF) on-demand disabled : static intra-DPU tasklet assignment
#   ON ) -D<PAT>_DYNAMIC    : idle tasklets steal work (on-demand)
# and dump per-DPU kernel_ms both times, so the distribution (spread, max/mean) shows
# how on-demand shrinks each DPU's tasklet straggler.
#   usage: CELL=CJ PATTERN=HOUSE5 bash exp_ondemand_perdpu.sh
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME=${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}
export PATH=$UPMEM_HOME/bin:$PATH
CELLS=${CELLS:-"PT PK CJ LJ"}; PATTERN=${PATTERN:-HOUSE5}
MK="NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16"
RUN="sudo stdbuf -oL -eL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"

# Barrier hybrid (verbatim GW_BD) + per-DPU host-observed profiling. NO early-promote.
GW_BD="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 \
-DOFIS_COLLECT_ON_FINAL_RETIRE -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
PROF="-DDPU_TIME_PROFILE"
DYN="-D${PATTERN}_DYNAMIC"   # on-demand ON

echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1; mkdir -p result
( while :; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & KA=$!
trap 'kill "$KA" 2>/dev/null' EXIT INT TERM
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="result/ondemandpd_${PATTERN}_$STAMP.log"; exec > >(tee "$LOG") 2>&1
echo "==> $LOG  CELLS='$CELLS' PATTERN=$PATTERN  (barrier + DPU_TIME_PROFILE)"

build(){ make clean >/dev/null 2>&1
  make -j16 all GRAPH=$CELL PATTERN=$PATTERN $MK EXTRA_DEFS="$1" >${TMPDIR:-/tmp}/pimgrasp.$$.od.build 2>&1 \
    || { echo "  BUILD FAIL: $(tail -4 ${TMPDIR:-/tmp}/pimgrasp.$$.od.build)"; return 1; }; }

# run once; write per-DPU CSV (gid,kernel_ms,idle_ms,xfer_ms,launches) + echo summary+e2e+ans
run_variant(){ local tag="$1" defs="$2" tmp=${TMPDIR:-/tmp}/pimgrasp.$$.od.out
  local csv="result/perdpu_${CELL}_${PATTERN}_${tag}_$STAMP.csv"
  build "$defs" || return 1
  $RUN 2>&1 | tee "$tmp" >/dev/null
  echo "dpu_id,kernel_ms,idle_ms,xfer_ms,launches" > "$csv"
  grep -aE '^DPUK ' "$tmp" | awk '{print $2","$3","$4","$5","$6}' >> "$csv"
  local n e a
  n=$(($(wc -l < "$csv") - 1))
  e=$(grep -aoE 'End-to-end Time \(ms\): [0-9.]+' "$tmp" | grep -oE '[0-9.]+' | tail -1)
  a=$(grep -aoE 'DPU ans: [0-9]+' "$tmp" | grep -oE '[0-9]+' | tail -1)
  echo "  [$tag] per-DPU rows=$n  E2E=${e:-?}ms  ans=${a:-?}  -> $csv"
  # quick spread stats on kernel_ms (active DPUs only)
  awk -F, 'NR>1 && $2>0 {v[n++]=$2; s+=$2; if($2>mx)mx=$2} END{
      if(n==0){print "    (no per-DPU kernel_ms -- light-only phase?)"; exit}
      m=s/n; for(i=0;i<n;i++){d=v[i]-m; ss+=d*d}; sd=sqrt(ss/n);
      printf "    kernel_ms: n=%d mean=%.2f max=%.2f max/mean=%.2f CV=%.3f\n", n,m,mx,mx/m,sd/m }' "$csv"
  echo "$csv"
}

stat(){ awk -F, 'NR>1 && $2>0 {s+=$2; n++; if($2>mx)mx=$2} END{printf "%.1f %.3f %.1f %d", s, (n?s/n:0), mx, n}' "$1"; }
declare -A R_SOFF R_MOFF R_XOFF R_SON R_MON R_XON
for CELL in $CELLS; do
  echo; echo "################  $CELL/$PATTERN  ################"
  echo "########  on-demand OFF (static tasklet)  ########"
  COFF=$(run_variant off "$GW_BD $PROF" | tail -1)
  echo "########  on-demand ON  (-D${PATTERN}_DYNAMIC)  ########"
  CON=$(run_variant on  "$GW_BD $PROF $DYN" | tail -1)
  read soff moff xoff noff <<<"$(stat "$COFF")"
  read son  mon  xon  non  <<<"$(stat "$CON")"
  R_SOFF[$CELL]=$soff; R_MOFF[$CELL]=$moff; R_XOFF[$CELL]=$xoff
  R_SON[$CELL]=$son;   R_MON[$CELL]=$mon;   R_XON[$CELL]=$xon
done

echo; echo "==============  DONE  =============="
echo "======  DPU EXECUTION TIME (kernel_ms, active DPUs) : on-demand OFF vs ON  ======"
printf "%-4s | %10s %10s %7s | %10s %10s %7s\n" "cell" "sumOFF" "sumON" "sum x" "maxOFF" "maxON" "max x"
CSV="result/ondemand_summary_${PATTERN}_$STAMP.csv"
echo "cell,sum_off_ms,sum_on_ms,sum_ratio,max_off_ms,max_on_ms,max_ratio" > "$CSV"
for CELL in $CELLS; do
  read sr mr <<<"$(awk -v so="${R_SOFF[$CELL]}" -v sn="${R_SON[$CELL]}" -v mo="${R_XOFF[$CELL]}" -v mn="${R_XON[$CELL]}" \
      'BEGIN{printf "%.3f %.3f", (sn>0?so/sn:0), (mn>0?mo/mn:0)}')"
  printf "%-4s | %10s %10s %6sx | %10s %10s %6sx\n" "$CELL" \
      "${R_SOFF[$CELL]}" "${R_SON[$CELL]}" "$sr" "${R_XOFF[$CELL]}" "${R_XON[$CELL]}" "$mr"
  echo "$CELL,${R_SOFF[$CELL]},${R_SON[$CELL]},$sr,${R_XOFF[$CELL]},${R_XON[$CELL]},$mr" >> "$CSV"
done
echo "summary CSV: $CSV"
echo "per-DPU CSVs: result/perdpu_<cell>_${PATTERN}_{off,on}_$STAMP.csv  (kernel_ms = per-DPU execution time)"
