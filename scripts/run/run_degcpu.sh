#!/bin/bash
# Per-node (degree, CPU-match-time) dump for the "degree anticipates per-root work" figure.
# Builds the host with -DDEGCYCLE_CPU: main.c then calls degcycle_cpu_run(), which matches the
# pattern on the CPU (multi-threaded) for EVERY root and writes, per node, "node,degree,ns".
# This path runs entirely on the CPU (no DPU launch), so it needs the UPMEM SDK only to compile.
#   host writes  src/result/degcpu_<PATTERN>_<DATA>.csv  (DATA = long name, e.g. cit-Patents)
#   plot: code/plot_deg_pat_cpu.py <DATA> <GRAPH>    (reads result/degcpu_<pat>_<DATA>.csv)
#
#   usage: bash scripts/run/run_degcpu.sh
#          CELLS="PT" PATTERNS="HOUSE5 P_P6" bash scripts/run/run_degcpu.sh
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)"  # bundle: build/run in paper_artifacts/src
UPMEM_HOME="${UPMEM_HOME:?set UPMEM_HOME to your UPMEM SDK path}"
export PATH="$UPMEM_HOME/bin:$PATH"
COMMON="NR_DPUS=1024 NR_TASKLETS=16 PIPELINE_PRODUCERS=16"
CELLS=${CELLS:-"PT"}; PATTERNS=${PATTERNS:-"HOUSE5 P_P6"}
DEFS="-DDEGCYCLE_CPU -DOFIS_SCALAR_ANS -DNO_PERF -DOFIS_LEAN"
# DEGCYCLE_CPU runs on the CPU only, so no sudo / DPU alloc is required.
RUN="env LD_LIBRARY_PATH=$UPMEM_HOME/lib DC_THREADS=${DC_THREADS:-32} DC_REPS=${DC_REPS:-1} ./bin/host"
mkdir -p result

for P in $PATTERNS; do for G in $CELLS; do
  echo "================= degcpu  $G / $P ================="
  make clean >/dev/null 2>&1
  if ! make -j16 all GRAPH=$G PATTERN=$P $COMMON EXTRA_DEFS="$DEFS" >"${TMPDIR:-/tmp}/pimgrasp.$$.dcpu.build" 2>&1; then
    echo "  BUILD FAIL: $(tail -4 "${TMPDIR:-/tmp}/pimgrasp.$$.dcpu.build")"; continue; fi
  $RUN 2>&1 | grep -aiE 'DEGCYCLE_CPU|wrote'
  [ "${PIPESTATUS[0]}" = 0 ] || echo "  RUN FAIL: $G / $P"
done; done
echo "==== DONE ====  CSVs: src/result/degcpu_<pat>_<data>.csv (-> bundle result/ via symlink)"
