#!/bin/bash
# Execution-time breakdown for the 3 headline engines, per graph/pattern.
#
# Source of Figures 1, 8, 9, 10, 11 and of the "with overlap" arm of Figure 15, so that they all
# show the same measurements (see scripts/parse/parse_breakdown.py). Keeps the full stdout
# of every run and parses the phase timers out of it.
#
# Every engine's segments sum EXACTLY to its End-to-end. The overlap engines
# collapse hidden work into "execution" by design, so for PIM-grasp we ALSO pull
# the compact-pipeline report: inline build = the compaction the worker had to do
# on the critical path (NOT overlapped, EXPOSED); producer build = the compaction
# a producer thread built ahead of time (HIDDEN behind execution). Only the
# exposed part enters the stacked bar; the hidden part is reported separately as
# an overlay ("this much build existed but was overlapped away").
#
# Six DIRECTLY-measured segments per engine (sum to E2E), no residual-lumping:
#   prep / comp(subgraph gen) / xfer(subgraph transfer) / exec(DPU op = wait, DIRECT)
#   / coll(result transfer = readback) / other(mux control + staging + gaps)
# exec is the measured DPU wait (PimPam: "DPU Time"; OFIS: wait_finish stage; PIM-grasp:
# ph.* wait= under the BARRIER hybrid) -- NOT op-minus-the-rest. PIM-grasp uses GW_BD
# (barrier) because early-promote reports wait=0 and folds DPU-busy into other.
# PIM-grasp also emits a "producer build (hidden)" overlay = compaction overlapped away.
#
#   usage: bash run_breakdown.sh
#          CELLS="PT CJ LJ" PATTERNS="HOUSE5 K4_TAIL5" REPS=1 bash run_breakdown.sh
#          LOGDIR=<dir> APPEND_CSV=1 ...   # add rows to <dir>/breakdown.csv instead of replacing it
set -u

# --- bundle: build/run in paper_artifacts/src; default UPMEM SDK ---
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"
UPMEM_HOME="${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}"
export PATH="$UPMEM_HOME/bin:$PATH"
CELLS=${CELLS:-"PT CJ LJ"}
PATTERNS=${PATTERNS:-"HOUSE5 K4_TAIL5"}     # heavy = HOUSE5, light = K4_TAIL5
DPUS=${DPUS:-"64 128 256 512 1024"}         # sweep DPU count (per-cell inner loop)
REPS=${REPS:-1}
ENGINES=${ENGINES:-"pimpam ofis pimgrasp"}
want(){ case " $ENGINES " in *" $1 "*) return 0;; *) return 1;; esac; }

MK="NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16"
RUN="sudo stdbuf -oL -eL env LD_LIBRARY_PATH=$UPMEM_HOME/lib ./bin/host"

# ---- engine flag sets ------------------------------------------------------
PP="-DOFIS_NO_MODE -DPIMPAM_COLLECT_PARALLEL -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
GS="-DOFIS_NO_COMPACT_PIPELINE -DOFIS_PREBUILD_ALL -DOFIS_PREBUILD_BATCH=1 -DOFIS_FIXED_BATCH=1 \
-DOFIS_COLLECT_PER_BATCH -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
GW="-DOFIS_HYBRID -DOFIS_HYBRID_EARLY_PROMOTE -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 \
-DOFIS_COLLECT_ON_FINAL_RETIRE -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
# BREAKDOWN uses the BARRIER hybrid (drop early-promote) so ph.* separates DPU wait
# (exec) from mux/staging (other) -- under early-promote wait=0 and everything folds
# into other. Barrier ~= early-promote in E2E, so the split is representative.
GW_BD="-DOFIS_HYBRID -DBATCH_TIER_BY_COUNT -DBATCH_DEG_BUDGET=256 \
-DGRAN_IG_BATCH_N=32 -DGRAN_RANK_BATCH_N=128 -DROOTS_PER_DPU=2048 \
-DOFIS_COLLECT_ON_FINAL_RETIRE -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"

if [ "${PARSE_ONLY:-0}" != 1 ]; then
  echo "Requesting sudo (DPU allocation requires root)..."; sudo -v || exit 1
  # Keep sudo alive: LJ cells + many clean builds exceed the 15-min sudo timestamp,
  # and an expiry mid-run makes ./bin/host fail (this clobbered logs repeatedly).
  ( while true; do sudo -n true 2>/dev/null || exit; sleep 50; done ) & SUDO_KA=$!
  trap 'kill "$SUDO_KA" 2>/dev/null' EXIT INT TERM
fi
mkdir -p result
STAMP=$(date +%Y%m%d-%H%M%S)
# REUSE=1 + LOGDIR=<existing dir> -> skip build+run for any cell whose log already
# has an End-to-end line (valid stdout), just re-parse it. Only missing cells run.
LOGDIR="${LOGDIR:-result/breakdown_$STAMP}"; mkdir -p "$LOGDIR"
have_log(){ [ -s "$1" ] && grep -qa 'End-to-end Time' "$1"; }
run_engine(){ # eng defs LOG g p r
  local eng="$1" defs="$2" L="$3" g="$4" p="$5" r="$6"
  if [ "${PARSE_ONLY:-0}" = 1 ]; then                                    # re-parse existing logs only
    if have_log "$L"; then parse "$eng" "$L" "$g" "$p" "$r"; else echo "    [$eng] no log: $(basename "$L")"; fi; return; fi
  if [ "${REUSE:-0}" = 1 ] && have_log "$L"; then echo "    [$eng] reuse $(basename "$L")"
  else build "$g" "$p" "$defs" || { echo "    [$eng] BUILD FAIL"; return; }
    # Write to a temp first; a mid-run sudo-credential expiry must NOT clobber a good log.
    local tmp="$L.tmp"; $RUN >"$tmp" 2>&1
    if grep -qa 'a password is required\|a terminal is required' "$tmp"; then
      echo "  !! sudo failed (non-interactive / credential expired). Run via '! ... bash run_breakdown.sh'. Aborting WITHOUT clobbering $(basename "$L")."; rm -f "$tmp"; exit 1; fi
    mv -f "$tmp" "$L"
  fi
  parse "$eng" "$L" "$g" "$p" "$r"
}
SUMMARY="$LOGDIR/summary.log"
if [ "${APPEND_CSV:-0}" = 1 ]; then exec > >(tee -a "$SUMMARY") 2>&1; else exec > >(tee "$SUMMARY") 2>&1; fi
CSV="$LOGDIR/breakdown.csv"
if [ "${APPEND_CSV:-0}" != 1 ] || [ ! -s "$CSV" ]; then
  echo "engine,graph,pattern,dpus,run,e2e_ms,prep_ms,comp_exposed_ms,xfer_ms,exec_ms,collect_ms,other_ms,hidden_build_ms,op_ms,ans,status,readsort_ms,task_ms" >"$CSV"
fi
echo "==> $LOGDIR   REPS=$REPS   numa_balancing=$(cat /proc/sys/kernel/numa_balancing 2>/dev/null)"

build(){ make clean >/dev/null 2>&1; make -j16 all GRAPH=$1 PATTERN=$2 $MK EXTRA_DEFS="$3" >"${TMPDIR:-/tmp}/pimgrasp.$$.bd.build" 2>&1 \
         || { echo "  BUILD FAIL: $(tail -4 "${TMPDIR:-/tmp}/pimgrasp.$$.bd.build")"; return 1; }; }

# --- metric helpers (grep the C printfs) ------------------------------------
m1(){ grep -aoE "$1 Time \(ms\): [0-9.]+" "$2" | grep -oE '[0-9.]+' | head -1; }   # first
msum(){ grep -aoE "$1 Time \(ms\): [0-9.]+" "$2" | grep -oE '[0-9.]+' \
        | awk '{s+=$1} END{if(NR)printf "%.6f", s}'; }                             # sum (per-round)
e2e_of(){ grep -aoE 'End-to-end Time \(ms\): [0-9.]+' "$1" | grep -oE '[0-9.]+' | tail -1; }
ans_of(){ grep -aoE 'DPU ans: [0-9]+' "$1" | grep -oE '[0-9]+' | tail -1; }
prebuild_of(){ grep -aF "[OFIS] bulk prebuild" "$1" 2>/dev/null | grep -oP "resident,\s*\K[0-9.]+" | tail -1; }
buildmax_of(){ grep -aF "$1" "$2" 2>/dev/null | grep -oP "max=\K[0-9.]+" | head -1; }   # per-thread WALL max
buildsum_of(){ grep -aF "$1" "$2" 2>/dev/null | grep -oP "sum=\K[0-9.]+" | head -1; }   # thread-AGGREGATE work
# phsum FIELD LOG -> sum of "FIELD=NUM" over the hybrid per-phase lines (ph.DPU/IG/RANK).
# These give the DPU-phase wall split into build / xfer / collect (survives OFIS_LEAN).
phsum(){ grep -aE '^  ph\.(DPU|IG|RANK)' "$2" 2>/dev/null | grep -oP "$1=\s*\K[0-9.]+" \
         | awk '{s+=$1} END{if(NR)printf "%.6f", s}'; }
# avg-thread ms of a named stage from the OFIS Stage Bottleneck Report (printf'd to
# stdout, non-hybrid too): e.g. compact_xfer (transfer), answer_collect (readback).
stage_of(){ grep -aE "^  $1 " "$2" 2>/dev/null | grep -oP 'avg=\s*\K[0-9.]+' | head -1; }
sub(){ awk -v a="${1:-0}" -v b="${2:-0}" 'BEGIN{printf "%.3f", a-b}'; }
status_of(){ if grep -qa "All fine" "$1"; then echo fine; elif grep -qa "DPU ans:" "$1"; then echo ok; else echo "?"; fi; }

# parse ENGINE LOG GRAPH PATTERN RUN  -> one CSV row, echoes a human line
parse(){
  local eng="$1" log="$2" g="$3" p="$4" r="$5"
  local e2e prep comp xfer exec coll other hidden op ans st
  e2e=$(e2e_of "$log"); ans=$(ans_of "$log"); st=$(status_of "$log")
  xfer=0; other=0; hidden=0; op=0
  # 6 DIRECTLY-measured segments (sum to E2E):
  #   prep / comp(subgraph gen) / xfer(subgraph transfer) / exec(DPU op = wait, DIRECT)
  #   / coll(result transfer = readback) / other(mux control + staging + gaps)
  case "$eng" in
    pimpam)   # static path -- all direct timers; host residual (mux/staging) folds into prep.
      comp=$(msum "Total Compacting" "$log");   comp="${comp:-0}"              # per-round -> sum
      xfer=$(msum "DPU data transfer" "$log");  xfer="${xfer:-0}"              # rCSR host->DPU push
      exec=$(m1 "DPU" "$log");                  exec="${exec:-0}"              # DPU Time (direct)
      coll=$(m1 "Answer collecting" "$log");    coll="${coll:-0}"              # result readback
      other=0
      prep=$(awk -v a="${e2e:-0}" -v c="$comp" -v xf="$xfer" -v x="$exec" -v k="$coll" \
             'BEGIN{printf "%.3f", a-c-xf-x-k}') ;;                            # read+sort+task-assign+staging
    ofis)     # prebuild-all: exec = wait_finish stage (DIRECT DPU wait); other = op residual.
      op=$(m1 "OFIS operation" "$log");         op="${op:-0}"
      comp=$(prebuild_of "$log");               comp="${comp:-0}"             # bulk prebuild
      xfer=$(stage_of compact_xfer "$log");     xfer="${xfer:-0}"             # subgraph transfer
      coll=$(stage_of answer_collect "$log");   coll="${coll:-0}"             # result readback
      exec=$(stage_of wait_finish "$log");      exec="${exec:-0}"             # DPU wait (DIRECT)
      other=$(awk -v o="$op" -v c="$comp" -v x="$xfer" -v k="$coll" -v e="$exec" \
              'BEGIN{v=o-c-x-k-e; if(v<0)v=0; printf "%.3f", v}')             # mux/staging/gaps
      prep=$(sub "$e2e" "$op") ;;
    pimgrasp) # BARRIER hybrid (GW_BD): ph.* separates DPU wait (exec) from mux/staging (other).
      # comp/xfer/coll = EXPOSED build/transfer/readback on the critical path (hidden build is
      # the separate producer overlay). exec = ph.* wait (DIRECT). other = ph.* other (mux etc).
      op=$(m1 "OFIS operation" "$log");   op="${op:-0}"
      comp=$(phsum build "$log");         comp="${comp:-0}"    # ph.* build   (exposed subgraph gen)
      xfer=$(phsum xfer "$log");          xfer="${xfer:-0}"    # ph.* xfer    (subgraph transfer)
      coll=$(phsum collect "$log");       coll="${coll:-0}"    # ph.* collect (result transfer)
      exec=$(phsum wait "$log");          exec="${exec:-0}"    # ph.* wait    (DPU op, DIRECT)
      other=$(awk -v o="$op" -v c="$comp" -v x="$xfer" -v k="$coll" -v e="$exec" \
              'BEGIN{v=o-c-x-k-e; if(v<0)v=0; printf "%.3f", v}')  # mux control + staging + gaps
      hidden=$(buildsum_of "producer build (hit)" "$log"); hidden="${hidden:-0}"  # overlapped-away overlay
      prep=$(sub "$e2e" "$op") ;;
  esac
  # read+sort and task assign alone (prep_ms also holds host staging for PimPam); task assign is PimPam-only.
  local readsort task
  readsort=$(awk -v a="$(msum "Data reading" "$log")" -v b="$(msum "Data Sorting" "$log")" 'BEGIN{printf "%.3f", a+b}')
  task=$(msum "Task Assign" "$log"); task="${task:-0}"
  echo "$eng,$g,$p,${D:-1024},$r,${e2e:-},$prep,$comp,$xfer,$exec,$coll,$other,$hidden,$op,${ans:-},$st,$readsort,$task" >>"$CSV"
  printf "    %-9s e2e=%-8s prep=%-7s sg-gen=%-7s sg-xfer=%-6s exec=%-8s res-xfer=%-6s other=%-7s hidden=%-8s (%s)\n" \
    "$eng" "${e2e:-NA}" "$prep" "$comp" "$xfer" "$exec" "$coll" "$other" "$hidden" "$st"
}

for P in $PATTERNS; do for G in $CELLS; do
  echo; echo "########################  $G / $P  ########################"
  Dpat="-D${P}_DYNAMIC"
  for D in $DPUS; do
    MK="NR_DPUS=$D NR_TASKLETS=16 PIPELINE_PRODUCERS=16"   # NR_DPUS swept here
    echo "  ---------------- ${D} DPUs ----------------"
    for r in $(seq 1 "$REPS"); do
      if want pimpam;   then build "$G" "$P" "$PP"          && { L="$LOGDIR/pimpam_${G}_${P}_${D}_r${r}.log";   $RUN >"$L" 2>&1; parse pimpam   "$L" "$G" "$P" "$r"; }; fi
      if want ofis;     then build "$G" "$P" "$GS"          && { L="$LOGDIR/ofis_${G}_${P}_${D}_r${r}.log";     $RUN >"$L" 2>&1; parse ofis     "$L" "$G" "$P" "$r"; }; fi
      if want pimgrasp; then build "$G" "$P" "$GW_BD $Dpat" && { L="$LOGDIR/pimgrasp_${G}_${P}_${D}_r${r}.log"; $RUN >"$L" 2>&1; parse pimgrasp "$L" "$G" "$P" "$r"; }; fi
    done
  done
  # count agreement across engines for this cell (ans = field 15; checked across all DPU counts)
  awk -F, -v g="$G" -v p="$P" 'NR>1 && $2==g && $3==p && $15!="" {u[$15]=1; last=$15}
    END{n=0; for(k in u)n++; if(n==0)print "  -> no ans"; else if(n==1)printf("  -> ANS MATCH: %s\n",last); else printf("  -> ** MISMATCH ** (%d distinct)\n",n)}' "$CSV"
done; done

echo; echo "======================================  BREAKDOWN CSV  ======================================"
column -s, -t "$CSV"
echo
echo "CSV: $CSV"
echo "segments (sum to E2E):"
echo "  PimPam    : prep(read+sort+task-assign) + comp + xfer + exec + collect"
echo "  OFIS/PG   : prep(read+sort) + comp + exec        (xfer folded into overlapped op -> hidden)"
echo "hidden_build = producer build (work-amount overlay, NOT in sum; PIM-grasp only)."
