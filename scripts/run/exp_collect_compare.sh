#!/bin/bash
# Result-collection comparison for P1 (HOUSE5) across PT/PK/CJ/LJ. Everything is held at
# the headline PIM-grasp config (run_breakdown.sh $GW, early-promote); ONLY the answer-
# collection mode is toggled (leave-one-out on collection):
#   PER  ) OFIS_COLLECT_PER_BATCH      = "per-batch": read each batch's result slice back as
#          soon as that batch finishes, on the dispatch critical path (original PimPam).
#   FR   ) OFIS_COLLECT_ON_FINAL_RETIRE = "final-retire": collect a DPU's results once, when it retires.
# Metric = "Answer collecting Time (ms)" (= OFIS_get_answer_collect_max_ms),
# plus E2E (per-batch collect sits on the critical path, so it can also move E2E) and ans.
#   usage: CELLS="PT PK CJ LJ" PATTERN=HOUSE5 REPS=1 bash exp_collect_compare.sh
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME=${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}
export PATH=$UPMEM_HOME/bin:$PATH
CELLS=${CELLS:-"PT PK CJ LJ"}; PATTERN=${PATTERN:-HOUSE5}; REPS=${REPS:-1}
# Which collection modes to actually MEASURE this run: "PER FR" (default, both), or e.g.
# MODES="FR" to only re-measure final-retire (batched) -- the PER (per-batch) rows are then
# REUSED verbatim from the newest complete collectcmp CSV (or $REF_CSV), so the output CSV
# stays complete and the plot can use it directly. PER is slow+stable; FR is fast (~8 ms).
MODES=${MODES:-"PER FR"}
REF_CSV=${REF_CSV:-}
# newest collectcmp CSV that has every cell x both modes (source for reused/skipped modes)
find_complete_ref(){ local c g m ok
  for c in $(ls -t result/collectcmp_${PATTERN}_*.csv 2>/dev/null); do
    ok=1; for g in PT PK CJ LJ; do for m in PER FR; do
      grep -q "^$g,$PATTERN,$m," "$c" || ok=0; done; done
    [ "$ok" = 1 ] && { echo "$c"; return; }
  done; }
# pull one cell/mode row (max mean sum e2e calls ans) from the reference CSV
ref_row(){ awk -F, -v c="$1" -v m="$2" '$1==c && $3==m {print $4,$5,$6,$7,$8,$9; exit}' "$REF_CSV"; }
MK="NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16"
RUN="sudo stdbuf -oL -eL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"

# FULL PIM-grasp with ALL optimizations ON, MINUS the collection flag (appended per variant).
# NOTE: early-promote (OFIS_HYBRID_EARLY_PROMOTE) is INCOMPATIBLE with per-batch collect
# (compile-time #error), so this comparison runs on the BARRIER hybrid (= GW_BD). Barrier
# keeps every other optimization on (batching, multi-granular, on-demand, overlap pipeline)
# and is E2E-equivalent to early-promote within ~1%; it is the only base where BOTH collect
# modes build, so it is the correct base to isolate the collection mode.
BASE="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 \
-D${PATTERN}_DYNAMIC \
-DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
PER="-DOFIS_COLLECT_PER_BATCH"          # per-batch
FR="-DOFIS_COLLECT_ON_FINAL_RETIRE"     # final-retire (headline)

echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1; mkdir -p result
( while :; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & KA=$!
trap 'kill "$KA" 2>/dev/null' EXIT INT TERM
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="result/collectcmp_${PATTERN}_$STAMP.log"; exec > >(tee "$LOG") 2>&1
echo "==> $LOG  REPS=$REPS  CELLS=$CELLS  PATTERN=$PATTERN  (collect mode leave-one-out)"

build(){ make clean >/dev/null 2>&1
  make -j16 all GRAPH=$1 PATTERN=$PATTERN $MK EXTRA_DEFS="$2" >${TMPDIR:-/tmp}/pimgrasp.$$.cc.build 2>&1 \
    || { echo "  BUILD FAIL: $(tail -4 ${TMPDIR:-/tmp}/pimgrasp.$$.cc.build)"; return 1; }; }

med(){ printf '%s\n' "$@" | grep -E '^[0-9.]+$' | sort -n | \
  awk '{a[NR]=$1} END{if(NR==0){print "?";exit} m=int((NR+1)/2);
        if(NR%2)printf "%.3f\n",a[m]; else printf "%.3f\n",(a[m]+a[m+1])/2}'; }

# echo: collect_max collect_mean collect_sum e2e calls ans  (MEDIAN over REPS; calls = number of batches).
# per-rep progress goes to stderr so $(best ...) captures only the final median line.
best(){ local defs="$1" G="$2" tag="$3" cx cn cs e a cc r i
  build "$G" "$defs" || { echo "FAIL FAIL FAIL FAIL 0 ?"; return; }
  local MX=() MN=() SM=() ES=() CC=() AS=()
  for r in $(seq 1 $REPS); do
    local tmp="result/collectraw_${G}_${tag}_${STAMP}_r$r.log"; $RUN 2>&1 | tee "$tmp" >/dev/null
    cx=$(grep -aoE 'Answer collecting Time \(ms\): [0-9.]+'  "$tmp" | grep -oE '[0-9.]+' | tail -1)
    cn=$(grep -aoE 'Answer collecting mean \(ms\): [0-9.]+'  "$tmp" | grep -oE '[0-9.]+' | tail -1)
    cs=$(grep -aoE 'Answer collecting sum \(ms\): [0-9.]+'   "$tmp" | grep -oE '[0-9.]+' | tail -1)
    e=$(grep -aoE 'End-to-end Time \(ms\): [0-9.]+'          "$tmp" | grep -oE '[0-9.]+' | tail -1)
    a=$(grep -aoE 'DPU ans: [0-9]+'                          "$tmp" | grep -oE '[0-9]+'  | tail -1)
    cc=$(grep -aE '^[[:space:]]*collect ' "$tmp" | tail -1 | grep -oE 'calls=[0-9]+' | cut -d= -f2)
    MX+=("$cx"); MN+=("$cn"); SM+=("$cs"); ES+=("$e"); CC+=("${cc:-0}"); AS+=("${a:-NA}")
    echo "    r$r: mean=${cn:-NA} max=${cx:-NA} sum=${cs:-NA} e2e=${e:-NA} calls=${cc:-NA} ans=${a:-NA}" >&2
  done
  # majority answer = "correct"; drop reps whose ans differs (stale/wedged DPU produced a
  # wrong count, e.g. a leftover result from a previous graph) so they can't pollute the median.
  local maj; maj=$(printf '%s\n' "${AS[@]}" | grep -E '^[0-9]+$' | sort | uniq -c | sort -rn | awk 'NR==1{print $2}')
  local vMX=() vMN=() vSM=() vES=() vCC=() ndrop=0
  for i in "${!AS[@]}"; do
    if [ -n "$maj" ] && [ "${AS[$i]}" = "$maj" ]; then
      vMX+=("${MX[$i]}"); vMN+=("${MN[$i]}"); vSM+=("${SM[$i]}"); vES+=("${ES[$i]}"); vCC+=("${CC[$i]}")
    else ndrop=$((ndrop+1)); fi
  done
  [ "$ndrop" -gt 0 ] && echo "    [dropped $ndrop/$REPS rep(s): ans != majority($maj) -- stale/wedged DPU]" >&2
  echo "$(med "${vMX[@]}") $(med "${vMN[@]}") $(med "${vSM[@]}") $(med "${vES[@]}") $(med "${vCC[@]}") ${maj:-?}"
}

# The CSV holds $EMIT_CELLS (default: the measured cells). Any (cell,mode) outside
# $CELLS x $MODES is pulled verbatim from a complete reference CSV.
EMIT_CELLS=${EMIT_CELLS:-$CELLS}
in_list(){ echo " $2 " | grep -q " $1 "; }   # in_list <item> <space-list>

# Need a reference CSV whenever something in the emit set won't be freshly measured.
need_ref=0
for G in $EMIT_CELLS; do in_list "$G" "$CELLS" || need_ref=1; done
in_list PER "$MODES" || need_ref=1
in_list FR  "$MODES" || need_ref=1
if [ "$need_ref" = 1 ]; then
  REF_CSV=${REF_CSV:-$(find_complete_ref)}
  [ -n "$REF_CSV" ] || { echo "ERROR: no complete collectcmp CSV to reuse un-measured (cell,mode); run MODES=\"PER FR\" CELLS=\"PT PK CJ LJ\" once first" >&2; exit 1; }
  echo "[reuse] un-measured (cell,mode) pulled verbatim from: $REF_CSV"
fi

declare -A PB FRr
for G in $EMIT_CELLS; do
  meas=0; in_list "$G" "$CELLS" && meas=1
  if [ "$meas" = 1 ] && in_list PER "$MODES"; then
    echo; echo "########  $G/$PATTERN : PER_BATCH (per-batch)  ########"
    PB["$G"]="$(best "$BASE $PER" "$G" PER)"
  else
    PB["$G"]="$(ref_row "$G" PER)"; echo "  [PER reused] $G"
  fi
  echo "  max mean sum e2e calls ans = ${PB[$G]}"
  if [ "$meas" = 1 ] && in_list FR "$MODES"; then
    echo; echo "########  $G/$PATTERN : FINAL_RETIRE (final-retire)  ########"
    FRr["$G"]="$(best "$BASE $FR" "$G" FR)"
  else
    FRr["$G"]="$(ref_row "$G" FR)"; echo "  [FR reused] $G"
  fi
  echo "  max mean sum e2e calls ans = ${FRr[$G]}"
done

CSV="result/collectcmp_${PATTERN}_$STAMP.csv"
echo "cell,pattern,mode,collect_max_ms,collect_mean_ms,collect_sum_ms,e2e_ms,collect_calls,ans" > "$CSV"
echo; echo "======  RESULT COLLECTION (ms, REPS=$REPS median): mean=per-rank, max=slowest rank, sum=total, calls=batches  ======"
echo "      measured: CELLS=[$CELLS] MODES=[$MODES]; the rest reused from ${REF_CSV:-'(none, all measured)'}"
printf "%-6s %-4s %10s %10s %10s %11s %11s\n" "cell" "mode" "mean" "max" "sum" "E2E" "calls"
for G in $EMIT_CELLS; do
  read px pn ps pe pc pa <<<"${PB[$G]:-? ? ? ? ? ?}"
  read fx fn fs fe fc fa <<<"${FRr[$G]:-? ? ? ? ? ?}"
  printf "%-6s %-4s %10s %10s %10s %11s %11s\n" "$G" "PER" "$pn" "$px" "$ps" "$pe" "$pc"
  printf "%-6s %-4s %10s %10s %10s %11s %11s  ans=%s\n" "$G" "FR" "$fn" "$fx" "$fs" "$fe" "$fc" \
         "$([ "$pa" = "$fa" ] && echo OK || echo "MISMATCH($pa/$fa)")"
  echo "$G,$PATTERN,PER,$px,$pn,$ps,$pe,$pc,$pa" >> "$CSV"
  echo "$G,$PATTERN,FR,$fx,$fn,$fs,$fe,$fc,$fa"  >> "$CSV"
done
echo "CSV: $CSV"
