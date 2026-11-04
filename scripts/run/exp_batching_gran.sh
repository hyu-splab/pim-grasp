#!/bin/bash
# Experiment 2: effect of DYNAMIC TASK BATCHING (+ multi-granular interaction) on the
# host-PIM communication. Reports light, heavy and total transfer time plus end-to-end time.
#
# -DGSDPU_BIN_THRESHOLD=16 splits each transfer into heavy(bin0)/light(bin1) by the
# batch's top-root degree, and the host prints "GSXFER light(deg<16)=... heavy=... ms".
# Wired inside compact_dispatch_masked, so it covers ALL three interaction
# granularities (DPU/IG/rank), not just the DPU-unit path.
#
# Two cumulative configs on the SAME barrier-hybrid engine (only batching differs;
# kernel, on-demand, and result collection are held fixed):
#   C1  no batching     : BATCH_DEG_BUDGET=1   -> every batch is 1 root (== per-task).
#   C3  + batching + granularity: BATCH_DEG_BUDGET=256, IG 32 / RANK 128.
#
# READING THE NUMBERS -- heavy is batch=1 / per-DPU in both configs, so it is invariant by design.
# The heavy column therefore serves (a) as a sanity check that measurement is correct, and
# (b) to show the fixed cost inside total/E2E that batching cannot touch.
# A smaller E2E improvement than light_xfer improvement is expected; that gap is exactly the
# size of "the part batching cannot touch (heavy transfer + kernel)".
#
#   usage: CELLS="PT CJ" PATTERNS="K4_TAIL5 HOUSE5" REPS=3 bash exp_batching_gran.sh
#          SELECT_BY=e2e|light   # which metric picks the best run across REPS (default e2e)
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME=${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}
export PATH=$UPMEM_HOME/bin:$PATH
CELLS=${CELLS:-"PT CJ"}; PATTERNS=${PATTERNS:-"K4_TAIL5 HOUSE5"}; REPS=${REPS:-1}
SELECT_BY=${SELECT_BY:-e2e}
RUN="sudo stdbuf -oL -eL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"

# barrier hybrid (NO early-promote) + the light/heavy transfer AND collect splitter (thr 16).
# Result collection is PER_BATCH here (not final-retire) so batching's effect on the
# result-collection cost is exposed the same way as its effect on the subgraph transfer.
BASE="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DOFIS_COLLECT_PER_BATCH \
-DGSDPU_BIN_THRESHOLD=16 -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
HUGE=1000000

echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1; mkdir -p result
( while :; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & KA=$!
trap 'kill "$KA" 2>/dev/null' EXIT INT TERM
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="result/expbatch_$STAMP.log"; exec > >(tee "$LOG") 2>&1
echo "==> $LOG  REPS=$REPS  CELLS=$CELLS  PATTERNS=$PATTERNS  SELECT_BY=$SELECT_BY"

build(){ make clean >/dev/null 2>&1
  make -j16 all GRAPH=$1 PATTERN=$2 NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16 \
       EXTRA_DEFS="$3" >${TMPDIR:-/tmp}/pimgrasp.$$.eb.build 2>&1 \
       || { echo "  BUILD FAIL: $(tail -3 ${TMPDIR:-/tmp}/pimgrasp.$$.eb.build)" >&2; return 1; }; }

# run once -> 10 fields:
#   e2e  lx hx  lc hc  lb hb  lcb hcb  ans
#   e2e = End-to-end Time (ms)
#   lx/hx  = subgraph transfer time, light / heavy (GSXFER bin1 / bin0)
#   lc/hc  = result collect time,    light / heavy (GSCOLLECT bin1 / bin0)
#   lb/hb  = # transferred batches,  light / heavy
#   lcb/hcb= # collected batches,    light / heavy
# heavy is batch=1 in both C1/C2 -> fluctuation signals a measurement/run anomaly.
run1(){ local tmp=${TMPDIR:-/tmp}/pimgrasp.$$.eb.out; $RUN 2>&1 | tee "$tmp" >/dev/null
  local e lx hx lc hc lb hb lcb hcb ans
  e=$(grep -aoE  'End-to-end Time \(ms\): [0-9.]+' "$tmp" | grep -oE '[0-9.]+' | tail -1)
  lx=$(grep -aoP 'GSXFER light\(deg<16\)=\K[0-9.]+'   "$tmp" | tail -1)
  hx=$(grep -aoP 'GSXFER.*?\bheavy=\K[0-9.]+'         "$tmp" | tail -1)
  lc=$(grep -aoP 'GSCOLLECT light\(deg<16\)=\K[0-9.]+' "$tmp" | tail -1)
  hc=$(grep -aoP 'GSCOLLECT.*?\bheavy=\K[0-9.]+'      "$tmp" | tail -1)
  lb=$(grep -a 'GSXFER'    "$tmp" | grep -oP 'light_batches=\K[0-9]+' | tail -1)
  hb=$(grep -a 'GSXFER'    "$tmp" | grep -oP 'heavy_batches=\K[0-9]+' | tail -1)
  lcb=$(grep -a 'GSCOLLECT' "$tmp" | grep -oP 'light_batches=\K[0-9]+' | tail -1)
  hcb=$(grep -a 'GSCOLLECT' "$tmp" | grep -oP 'heavy_batches=\K[0-9]+' | tail -1)
  ans=$(grep -aoE 'DPU ans: [0-9]+' "$tmp" | grep -oE '[0-9]+' | tail -1)
  echo "${e:-0} ${lx:-0} ${hx:-0} ${lc:-0} ${hc:-0} ${lb:-0} ${hb:-0} ${lcb:-0} ${hcb:-0} ${ans:-?}"; }

# pick one best run across REPS, by SELECT_BY (e2e=field 1, light=field 2).
best(){ local defs="$1" G="$2" P="$3" r line key bkey="" bline=""
  build "$G" "$P" "$defs" || { echo "0 FAIL 0 0 0 0 0 0 0 ?"; return; }
  local fld=1; [ "$SELECT_BY" = "light" ] && fld=2
  for r in $(seq 1 $REPS); do
    line=$(run1); key=$(echo "$line" | cut -d' ' -f$fld)
    echo "    r$r: e2e=$(echo "$line"|cut -d' ' -f1) light_xfer=$(echo "$line"|cut -d' ' -f2)" \
         "heavy_xfer=$(echo "$line"|cut -d' ' -f3) ans=$(echo "$line"|cut -d' ' -f10)" >&2
    if [ "$key" != "0" ] && { [ -z "$bkey" ] || awk -v a="$bkey" -v x="$key" 'BEGIN{exit !(x<a)}'; }; then
      bkey="$key"; bline="$line"; fi
  done
  echo "${bline:-0 FAIL 0 0 0 0 0 0 0 ?}"; }

declare -A R
for P in $PATTERNS; do for G in $CELLS; do
  echo; echo "########################  $G / $P  ########################"
  DYN="-D${P}_DYNAMIC -D${P}_MAX_BATCH_ROOTS=2048"
  echo "----- C1: no batching (batch=1) -----"
  R["$G/$P/C1"]="$(best "$BASE $DYN -DBATCH_DEG_BUDGET=1" "$G" "$P")"
  echo "  C1 = ${R[$G/$P/C1]}"
  echo "----- C3: + batching + interaction granularity (hybrid: IG 32 / RANK 128) -----"
  R["$G/$P/C3"]="$(best "$BASE $DYN -DBATCH_DEG_BUDGET=256 -DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128" "$G" "$P")"
  echo "  C3 = ${R[$G/$P/C3]}"
done; done

CSV="result/expbatch_$STAMP.csv"
echo "cell,pattern,config,e2e_ms,xfer_total_ms,light_xfer_ms,heavy_xfer_ms,collect_total_ms,light_collect_ms,heavy_collect_ms,batches_total,light_batches,heavy_batches,collect_batches_total,ans" > "$CSV"

echo
echo "======  host-PIM comm + E2E (all tasks incl. heavy)  ======"
printf "%-11s %-12s %10s %10s %10s %10s %10s %10s %10s %9s   %s\n" \
  "cell/pat" "config" "E2E" "xfer_tot" "  xfer_lt" "  xfer_hv" "coll_tot" "  coll_lt" "  coll_hv" "batches" "ans"
for P in $PATTERNS; do for G in $CELLS; do
  for cfg in C1 C3; do
    read e lx hx lc hc lb hb lcb hcb a <<<"${R[$G/$P/$cfg]:-0 ? 0 0 0 0 0 0 0 ?}"
    case $cfg in C1) nm="C1 no-batch";; C3) nm="C3 +batch+gran";; esac
    read xt ct bt cbt <<<"$(awk -v a="$lx" -v b="$hx" -v c="$lc" -v d="$hc" \
        -v e="$lb" -v f="$hb" -v g="$lcb" -v h="$hcb" \
        'BEGIN{printf "%.1f %.1f %d %d", a+b, c+d, e+f, g+h}')"
    printf "%-11s %-12s %10s %10s %10s %10s %10s %10s %10s %9s   %s\n" \
      "$G/$P" "$nm" "$e" "$xt" "$lx" "$hx" "$ct" "$lc" "$hc" "$bt" "$a"
    echo "$G,$P,$cfg,$e,$xt,$lx,$hx,$ct,$lc,$hc,$bt,$lb,$hb,$cbt,$a" >> "$CSV"
  done

  read e1 l1 h1 c1 hc1 n1 hn1 cb1 hcb1 a1 <<<"${R[$G/$P/C1]:-0 ? 0 0 0 0 0 0 0 ?}"
  read e2 l2 h2 c2 hc2 n2 hn2 cb2 hcb2 a2 <<<"${R[$G/$P/C3]:-0 ? 0 0 0 0 0 0 0 ?}"
  awk -v e1="$e1" -v e2="$e2" -v l1="$l1" -v l2="$l2" -v h1="$h1" -v h2="$h2" \
      -v c1="$c1" -v c2="$c2" -v hc1="$hc1" -v hc2="$hc2" \
      -v n1="$n1" -v n2="$n2" -v hn1="$hn1" -v hn2="$hn2" -v a1="$a1" -v a2="$a2" 'BEGIN{
    if(e1+0>0&&e2+0>0) printf "            E2E           %.2fx (%.0f->%.0f ms)   <- overall\n", e1/e2, e1, e2;
    if(l1+0>0&&l2+0>0) printf "            xfer  light   %.2fx (%.0f->%.0f ms)\n", l1/l2, l1, l2;
    if(h1+0>0&&h2+0>0){ printf "            xfer  heavy   %.2fx (%.0f->%.0f ms)", h1/h2, h1, h2;
                        printf (h1/h2>1.1||h1/h2<0.9) ? "   <- WARN: heavy should be invariant\n" : "   (invariant, as expected)\n"; }
    if(l1+h1>0&&l2+h2>0) printf "            xfer  TOTAL   %.2fx (%.0f->%.0f ms)\n", (l1+h1)/(l2+h2), l1+h1, l2+h2;
    if(c1+hc1>0&&c2+hc2>0) printf "            coll  TOTAL   %.2fx (%.0f->%.0f ms)\n", (c1+hc1)/(c2+hc2), c1+hc1, c2+hc2;
    if(n1+hn1>0&&n2+hn2>0) printf "            batches TOTAL %.2fx (%d->%d)   <- should track the reduction above\n", (n1+hn1)/(n2+hn2), n1+hn1, n2+hn2;
    if(a1!=a2) printf "            !! ans MISMATCH (%s / %s) -- this cell is unreliable\n", a1, a2;
    if(e1+0>0&&e2+0>0&&l1+0>0&&l2+0>0)
      printf "            [E2E %.1f%% vs light_xfer %.1f%% reduction -- gap = heavy+kernel share batching cannot touch]\n",
             (e1-e2)/e1*100, (l1-l2)/l1*100 }'
done; done
echo
echo "CSV: $CSV"