#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// PERF: per-root cycle-count instrumentation; inflates timing, so use -DNO_PERF for timing runs.
#ifndef NO_PERF
#define PERF
#endif
// #define DPU_LOG
// #define CPU_RUN
#define NO_PARTITION_AS_POSSIBLE
//#define MORE_ACCURATE_MODEL


#if defined(CLIQUE4) || defined(CLIQUE5)
#define BITMAP
#endif

#define DATA_DIR "./data/bin/"
#if defined(SELF)
#define DATA_NAME "self-defined"
#define N (1<<5)
#define M (1<<5)
#elif defined(WV)
#define DATA_NAME "Wiki-Vote"
#define N (1<<13)
#define M (1<<18)
#elif defined(PP)
#define DATA_NAME "p2p-Gnutella04"
#define N (1<<14)
#define M (1<<17)
#elif defined(CA)
#define DATA_NAME "CA-AstroPh"
#define N (1<<15)
#define M (1<<20)
#elif defined(YT)
#define DATA_NAME "com-youtube"
#define N (1<<21)
#define M (1<<23)
#elif defined(PT)
#define DATA_NAME "cit-Patents"
#define N (1<<22)
#define M (1<<26)
#elif defined(CJ)
#define DATA_NAME "com-LiveJournal"
#define N (1<<22)
#define M (1<<27)
#elif defined(LJ)
#define DATA_NAME "soc-LiveJournall"
#define N (1<<23)
#define M (1<<27)
#elif defined(WT)
#define DATA_NAME "WikiTalk"
#define N (1<<22)
#define M (1<<24)
#elif defined(PK)
// soc-Pokec: large (1.63M nodes, ~30.6M directed edges), low degree skew.
#define DATA_NAME "soc-Pokec"
#define N (1<<21)
#define M (1<<26)

#elif defined(SYNTH)
// Synthetic graphs: name and N/M bounds supplied per graph via -DSYNTH_NAME/-DSYNTH_N_LOG/-DSYNTH_M_LOG.
#if !defined(SYNTH_NAME) || !defined(SYNTH_N_LOG) || !defined(SYNTH_M_LOG)
#error "GRAPH=SYNTH requires -DSYNTH_NAME, -DSYNTH_N_LOG and -DSYNTH_M_LOG"
#endif
#define DATA_NAME SYNTH_NAME
#define N (1<<SYNTH_N_LOG)
#define M (1<<SYNTH_M_LOG)

#else
#warning "No graph selected, fall back to PP."
#define DATA_NAME "p2p-Gnutella04"
#define N (1<<14)
#define M (1<<17)
#endif
#define DATA_PATH DATA_DIR DATA_NAME ".bin"


#ifndef NR_DPUS
#warning "No NR_DPUS defined, fall back to 1."
#define NR_DPUS 1
#endif
#ifndef NR_TASKLETS
#warning "No NR_TASKLETS defined, fall back to 1."
#define NR_TASKLETS 1
#endif
#ifndef DPU_BINARY
#warning "No DPU_BINARY defined, fall back to bin/dpu."
#define DPU_BINARY "bin/dpu"
#endif
#ifndef DPU_ALLOC_BINARY
#warning "No DPU_ALLOC_BINARY defined, fall back to bin/dpu_alloc."
#define DPU_ALLOC_BINARY "bin/dpu_alloc"
#endif

#if defined(CLIQUE2)
#define KERNEL_FUNC clique2
#define PATTERN_NAME "clique2"
#elif defined(CLIQUE3)
#define KERNEL_FUNC clique3
#define PATTERN_NAME "clique3"
#elif defined(CLIQUE4)
#define KERNEL_FUNC clique4
#define PATTERN_NAME "clique4"
#elif defined(CLIQUE5)
#define KERNEL_FUNC clique5
#define PATTERN_NAME "clique5"
#elif defined(CYCLE4)
#define KERNEL_FUNC cycle4
#define PATTERN_NAME "cycle4"
#elif defined(HOUSE5)
#define KERNEL_FUNC house5
#define PATTERN_NAME "house5"
#elif defined(TRI_TRI6)
#define KERNEL_FUNC tri_tri6
#define PATTERN_NAME "tri_tri6"
#elif defined(QG3)
#define KERNEL_FUNC qg3
#define PATTERN_NAME "qg3"
#elif defined(PENTAGON)
#define KERNEL_FUNC pentagon
#define PATTERN_NAME "pentagon"
#elif defined(HOURGLASS)
#define KERNEL_FUNC hourglass
#define PATTERN_NAME "hourglass"
#elif defined(K4_TAIL5)
#define KERNEL_FUNC k4_tail5
#define PATTERN_NAME "k4_tail5"
#elif defined(P_P6)
#define KERNEL_FUNC p_p6
#define PATTERN_NAME "p_p6"
#elif defined(HOUSE5_X)
#define KERNEL_FUNC house5_x
#define PATTERN_NAME "house5_x"
#elif defined(HOUSE5_Y)
#define KERNEL_FUNC house5_y
#define PATTERN_NAME "house5_y"
#else
#warning "No kernel function selected, fall back to house5."
#define KERNEL_FUNC house5
#define PATTERN_NAME "house5"
#endif

#define node_t uint32_t
#define edge_ptr uint32_t
#define ans_t uint64_t
#define SIZE_NODE_T_LOG 2
#define SIZE_EDGE_PTR_LOG 2
#define INVALID_NODE ((node_t)(-1))
#define DPU_N ((1<<24)/sizeof(edge_ptr))
#define DPU_M ((1<<25)/sizeof(node_t))
#define DPU_ROOT_NUM ((1<<20)/sizeof(node_t))
#define BITMAP_SIZE 32  // 1024 bits
#define BUF_SIZE 32
// Per-tasklet MRAM scratch for one root's neighbour list; caps max degree on the STATIC path. -D overridable.
#ifndef MRAM_BUF_SIZE
#define MRAM_BUF_SIZE 32768
#endif
// Degree thresholds (default 16), each sweepable: BRANCH (branch-level collab), BATCH_HEAVY (heavy batch), GRAN_SPLIT/GRAN_RANK (HYBRID DPU/IG/RANK routing).
#ifndef BRANCH_LEVEL_THRESHOLD
#define BRANCH_LEVEL_THRESHOLD 16
#endif
#ifndef BATCH_HEAVY_THRESHOLD
#define BATCH_HEAVY_THRESHOLD 16
#endif
#ifndef GRAN_SPLIT_THRESHOLD
#define GRAN_SPLIT_THRESHOLD 16
#endif
#ifndef GRAN_RANK_THRESHOLD
#define GRAN_RANK_THRESHOLD 0
#endif
#define PARTITION_M ((1<<22)/sizeof(node_t))
#ifndef OFIS_NO_MODE
#define OFIS_MODE   /* selective refill; disable for the static baseline with -DOFIS_NO_MODE */
#endif
// OFIS_FIXED_BATCH: pin the refill batch to a constant (batch-size study); OFIS_MODE only.
#ifndef OFIS_FIXED_BATCH
#define OFIS_FIXED_BATCH 16
#endif
// CPU_COMPACTION: STATIC path only; build each DPU's compact CSR on the host CPU (no effect under OFIS_MODE).
// CPU compaction thread count; used only when CPU_COMPACTION is defined.
#ifndef CPU_COMPACTION_THREADS
#define CPU_COMPACTION_THREADS 32
#endif
// OFIS dispatch/finish granularity: OFIS_DPU (default) or OFIS_HYBRID.
#if !defined(OFIS_DPU) && !defined(OFIS_HYBRID)
#define OFIS_DPU
#endif
#ifndef OFIS_NO_THREAD_INIT_CSR_XFER
// init-CSR overlap: split the initial CSR transfer across OFIS workers; resident-graph path only.
#define OFIS_THREAD_INIT_CSR_XFER
#endif
// OFIS_RUNTIME_LOG=1: per-worker progress dump every 2s from the join monitor.
#ifndef OFIS_RUNTIME_LOG
#define OFIS_RUNTIME_LOG 0
#endif

// compaction overlap: host-side parallel rCSR build via a producer-fed prefetch queue; non-resident path only, disable with -DOFIS_NO_COMPACT_PIPELINE.
#ifndef OFIS_NO_COMPACT_PIPELINE
#define OFIS_COMPACT_PIPELINE
#endif
// Queue depth derived from NR_DPUS so producers never block on push (2x headroom).
#ifndef OFIS_COMPACT_PIPELINE_QUEUE_DEPTH
#define OFIS_COMPACT_PIPELINE_QUEUE_DEPTH (NR_DPUS * 2)
#endif
#ifndef OFIS_COMPACT_PIPELINE_PRODUCERS
#define OFIS_COMPACT_PIPELINE_PRODUCERS MAX_N_RANK
#endif

// OFIS_PREBUILD_ALL: PimPam-style baseline; build every batch's rCSR up front in one bulk phase (no overlap), non-resident path only.
#if defined(OFIS_PREBUILD_ALL) && defined(OFIS_COMPACT_PIPELINE)
#error "OFIS_PREBUILD_ALL replaces the prefetch pipeline; build it with -DOFIS_NO_COMPACT_PIPELINE."
#endif
// Batch size for the bulk partition; the baseline pins it to 1 (no batching amortization).
#ifndef OFIS_PREBUILD_BATCH
#define OFIS_PREBUILD_BATCH 1
#endif
// Host threads for the bulk build (matches CPU_COMPACTION_THREADS).
#ifndef OFIS_PREBUILD_THREADS
#define OFIS_PREBUILD_THREADS 32
#endif
// Pipeline and bulk prebuild both feed the worker a ready-made payload; shared path.
#if defined(OFIS_COMPACT_PIPELINE) || defined(OFIS_PREBUILD_ALL)
#define OFIS_PAYLOAD_PATH
#endif

// Answer collection, exactly one: OFIS_COLLECT_AT_END (one post-run sweep), OFIS_COLLECT_ON_FINAL_RETIRE (at DPU retire), OFIS_COLLECT_PER_BATCH (per batch, original PimPam).
#if (defined(OFIS_COLLECT_AT_END) + defined(OFIS_COLLECT_ON_FINAL_RETIRE) + defined(OFIS_COLLECT_PER_BATCH)) > 1
#error "Choose only one OFIS answer collection mode."
#endif
#if !defined(OFIS_COLLECT_AT_END) && !defined(OFIS_COLLECT_ON_FINAL_RETIRE) && !defined(OFIS_COLLECT_PER_BATCH)
#define OFIS_COLLECT_AT_END
#endif

#define MAX_N_RANK (NR_DPUS/64)
#define MAX_N_THREAD MAX_N_RANK

#ifdef OFIS_HYBRID
#if !defined(OFIS_PAYLOAD_PATH)
#error "OFIS_HYBRID requires the compact (non-resident) payload path: the default OFIS_COMPACT_PIPELINE (all-on) or -DOFIS_PREBUILD_ALL -DOFIS_NO_COMPACT_PIPELINE (all-off)."
#endif
#if defined(OFIS_NO_MODE)
#error "OFIS_HYBRID needs the DPU state loop (OFIS_MODE); do not build with -DOFIS_NO_MODE."
#endif
#endif

// Optimization-flag dependency guards. Each optimization (overlap Pc/Pi/Co, refill
// R, task batching BATCH_DEG_BUDGET, hybrid granularity OFIS_HYBRID, intra-DPU
// on-demand <PAT>_DYNAMIC) is an independent -D axis; enabling one never enables
// another. The flags below only refine a prerequisite, so they error loudly rather
// than silently do nothing. For hierarchy WITHOUT batching, use the per-root
// degree-cut: -DOFIS_HYBRID -DGRAN_SPLIT_THRESHOLD=N -DGRAN_RANK_THRESHOLD=M.
#if defined(BATCH_TIER_BY_COUNT) && !defined(BATCH_DEG_BUDGET)
#error "BATCH_TIER_BY_COUNT needs BATCH_DEG_BUDGET (a tier is the root count of a degree-budget batch). For hierarchy without batching use OFIS_HYBRID with -DGRAN_SPLIT_THRESHOLD/-DGRAN_RANK_THRESHOLD."
#endif
#if defined(TASKLET_IDLE) && !defined(PERF)
#error "TASKLET_IDLE requires PERF -- idle is accumulated into the PERF-only cycle_ct[] array, which NO_PERF does not build. Drop -DNO_PERF."
#endif

typedef struct Graph {
    node_t n;  // number of vertices
    edge_ptr m;  // number of edges
    edge_ptr row_ptr[N];
    node_t col_idx[M];
    uint64_t root_num[NR_DPUS];  // number of search roots allocated to dpu
    node_t *roots[NR_DPUS];
} Graph;

#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#define ALIGN2(x) ALIGN(x, 2)
#define ALIGN4(x) ALIGN(x, 4)
#define ALIGN8(x) ALIGN(x, 8)
#define ALIGN16(x) ALIGN(x, 16)
#define ALIGN_LOWER(x, a) ((x) & ~((a)-1))
#define ALIGN2_LOWER(x) ALIGN_LOWER(x, 2)
#define ALIGN4_LOWER(x) ALIGN_LOWER(x, 4)
#define ALIGN8_LOWER(x) ALIGN_LOWER(x, 8)
#define ALIGN16_LOWER(x) ALIGN_LOWER(x, 16)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"



#endif // COMMON_H
