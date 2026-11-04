#include <stdlib.h>
#include <string.h>   // memcpy (host buffers)
#include <assert.h>
#include <dpu.h>
#include <ofis.h>
#include <dpu_types.h>
#include <common.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>   // printf("%" PRIu64, ...)
#include <sched.h>
#include <errno.h>
#include <unistd.h>

#define OPEN_TO_DPU 0
#define OPEN_TO_HOST 1

uint32_t state_var = 0;

static double g_wait_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_barrier_wait_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_assign_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_mux_host_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_state_update_ms_by_thread[MAX_N_THREAD] = {0.0};

static double g_hybrid_phase_wall_ms[3][MAX_N_THREAD] = {{0.0}};
static double g_hybrid_phase_wait_ms[3][MAX_N_THREAD] = {{0.0}};
static double g_hybrid_phase_xfer_ms[3][MAX_N_THREAD]      = {{0.0}};
static double g_hybrid_phase_build_ms[3][MAX_N_THREAD]     = {{0.0}};
static double g_hybrid_phase_coll_ms[3][MAX_N_THREAD]      = {{0.0}};
#ifdef DPU_TIME_PROFILE
// Per-DPU kernel wall (host-observed finish-launch) over DPU-tier on-demand refills; heavy phase only.
static double         g_dpu_kernel_ms [NR_DPUS] = {0.0};   /* busy: sum(finish-launch) */
static double         g_dpu_idle_ms   [NR_DPUS] = {0.0};   /* host-wait: sum(relaunch-finish) */
static double         g_dpu_xfer_ms   [NR_DPUS] = {0.0};   /* subgraph-transfer wall this DPU waited on */
static uint32_t       g_dpu_launch_cnt[NR_DPUS] = {0};
static struct timeval g_dpu_launch_tv [NR_DPUS];
static struct timeval g_dpu_finish_tv [NR_DPUS];
#endif
// -DPIMPAM_PERDPU_FINISH: static PimPam per-DPU finish wall (ms) via async launch + dpu_status polling, summed across rounds.
#ifdef PIMPAM_PERDPU_FINISH
static double         g_perdpu_finish_ms[NR_DPUS] = {0.0};
#endif
static uint64_t g_hybrid_phase_pophit[3][MAX_N_THREAD]     = {{0}};
static uint64_t g_hybrid_phase_popmiss[3][MAX_N_THREAD]    = {{0}};
#ifdef GSDPU_BIN_THRESHOLD
/* Host-PIM communication, split by the top-root degree of each DPU-batch:
 * index 0 = heavy (degree >= GSDPU_BIN_THRESHOLD), index 1 = light. */
#define GSDPU_DEG(v)    ((edge_ptr)(global_g->row_ptr[(v)+1] - global_g->row_ptr[(v)]))
#define GSDPU_BIN(rptr) ((GSDPU_DEG((rptr)[0]) >= (edge_ptr)(GSDPU_BIN_THRESHOLD)) ? 0 : 1)
static double   g_gsxfer_ms[2][MAX_N_THREAD]    = {{0.0}};
static uint64_t g_gsxfer_nb[2][MAX_N_THREAD]    = {{0}};
static double   g_gscollect_ms[2][MAX_N_THREAD] = {{0.0}};
static uint64_t g_gscollect_nb[2][MAX_N_THREAD] = {{0}};
#endif

#ifndef REGION_CUT1
#define REGION_CUT1 32
#endif
#ifndef REGION_CUT2
#define REGION_CUT2 128
#endif
static uint64_t g_region_xfer_bytes[3][MAX_N_THREAD] = {{0}};
static double   g_region_build_ms[3][MAX_N_THREAD]   = {{0.0}};
static double   g_region_xfer_ms[3][MAX_N_THREAD]    = {{0.0}};
static uint64_t g_region_batches[3][MAX_N_THREAD]    = {{0}};
static double   g_hybrid_phase_strag_sum_ms[3][MAX_N_THREAD] = {{0.0}};
static double   g_hybrid_phase_strag_max_ms[3][MAX_N_THREAD] = {{0.0}};
static uint64_t g_hybrid_phase_strag_cnt[3][MAX_N_THREAD]    = {{0}};   /* # rounds, for per-round avg */
static double g_hybrid_phase_tail_ms[3][MAX_N_THREAD]     = {{0.0}};
static double g_hybrid_phase_idle_dpu_ms[3][MAX_N_THREAD] = {{0.0}};
static double g_hybrid_trans_gap_ms[3][MAX_N_THREAD]      = {{0.0}};


static struct timeval g_parallel_t0;
static double g_first_launch_ms_by_thread[MAX_N_THREAD] = {0.0};
static double  g_init_build_sum_ms_by_thread[MAX_N_THREAD] = {0.0};
static double  g_init_build_max_ms_by_thread[MAX_N_THREAD] = {0.0};
static uint32_t g_init_inline_cnt_by_thread[MAX_N_THREAD]  = {0};
static uint32_t g_init_dpu_cnt_by_thread[MAX_N_THREAD]     = {0};
static double  g_init_transfer_ms_by_thread[MAX_N_THREAD]    = {0.0};
#ifdef OFIS_STARTUP_BARRIER
static pthread_barrier_t g_startup_barrier;
#define OFIS_STARTUP_SYNC() pthread_barrier_wait(&g_startup_barrier)
#else
#define OFIS_STARTUP_SYNC() ((void)0)
#endif
#define OFIS_STAMP_FIRST_LAUNCH(tid) do { struct timeval _fl; gettimeofday(&_fl, NULL); \
    g_first_launch_ms_by_thread[(tid)] = get_time_in_milliseconds(&g_parallel_t0, &_fl); } while(0)

static double   g_pcount_lock_wait_ms_by_thread[MAX_N_THREAD] = {0.0};
static double   g_pcount_cs_hold_ms_by_thread [MAX_N_THREAD] = {0.0};
static uint64_t g_pcount_lock_acqs_by_thread  [MAX_N_THREAD] = {0};

static double g_t_rootnum_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_t_offset_ms_by_thread  [MAX_N_THREAD] = {0.0};
static double g_t_roots_ms_by_thread   [MAX_N_THREAD] = {0.0};
static double g_compact_build_ms_by_thread[MAX_N_THREAD] = {0.0};
static double g_compact_xfer_ms_by_thread [MAX_N_THREAD] = {0.0};
static uint64_t g_compact_batch_count_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_compact_row_bytes_by_thread  [MAX_N_THREAD] = {0};
static uint64_t g_compact_col_bytes_by_thread  [MAX_N_THREAD] = {0};
static uint64_t g_bcb_total_edges = 0;
static uint64_t g_bcb_total_nodes = 0;
static uint64_t g_bcb_calls       = 0;

static uint64_t g_collect_rounds_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_collect_calls_by_thread [MAX_N_THREAD] = {0};
static uint64_t g_collect_bytes_by_thread [MAX_N_THREAD] = {0};

#ifdef OFIS_COLLECT_PER_BATCH
static uint64_t *g_dpu_result_vals[NR_DPUS];
static uint64_t  g_dpu_result_cap [NR_DPUS];

static void ofis_dpu_result_reserve(uint32_t gd, uint64_t need) {
    if (need <= g_dpu_result_cap[gd]) return;
    const uint64_t old_cap = g_dpu_result_cap[gd];
    uint64_t cap = old_cap ? old_cap : 64;
    while (cap < need) cap <<= 1;
    uint64_t *grown = realloc(g_dpu_result_vals[gd], (size_t)cap * sizeof(uint64_t));
    if (!grown) {
        printf(ANSI_COLOR_RED "Error: per-DPU result mirror grow failed (dpu %u)\n"
               ANSI_COLOR_RESET, gd);
        exit(1);
    }
    g_dpu_result_vals[gd] = grown;
    g_dpu_result_cap[gd]  = cap;
}
#endif /* OFIS_COLLECT_PER_BATCH */

// call counts for averaging
static unsigned long long g_c_rootnum_by_thread[MAX_N_THREAD] = {0};
static unsigned long long g_c_offset_by_thread  [MAX_N_THREAD] = {0};
static unsigned long long g_c_roots_by_thread   [MAX_N_THREAD] = {0};
static uint64_t g_n_rootnum_dpu_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_n_offset_dpu_by_thread [MAX_N_THREAD] = {0};
static uint64_t g_n_roots_dpu_by_thread  [MAX_N_THREAD] = {0};
static uint64_t g_b_rootnum_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_b_offset_by_thread [MAX_N_THREAD] = {0};
static uint64_t g_b_roots_by_thread  [MAX_N_THREAD] = {0};
static uint64_t g_c_state_update_calls_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_c_state_update_dpus_by_thread [MAX_N_THREAD] = {0};

// execution time evaluating variable for ig
static double g_ig_cycle_sum[MAX_N_THREAD][8] = {0.0};   // per-IG cycle time sum
static uint64_t g_ig_cycle_count[MAX_N_THREAD][8] = {0}; // per-IG completion count
static double g_ig_cycle_max[MAX_N_THREAD][8] = {0.0};   // per-IG max cycle time

// execution time evaluating variable for dpu
static double g_dpu_cycle_sum[MAX_N_THREAD][64] = {0.0};   // per-DPU cycle time sum
static uint64_t g_dpu_cycle_count[MAX_N_THREAD][64] = {0}; // per-DPU completion count
// Per-DPU hardware cycle load (sum of per-root cycle_ct over all batches); PERF-only.
static uint64_t g_dpu_cycle_ct[MAX_N_THREAD][64] = {{0}};
static double g_dpu_cycle_max[MAX_N_THREAD][64] = {0.0};   // per-DPU max cycle time
static double g_init_rowptr_ms_by_thread[MAX_N_THREAD] = {0.0};  // thread-local initial row_ptr transfer
static double g_init_colidx_ms_by_thread[MAX_N_THREAD] = {0.0};  // thread-local initial col_idx transfer
static double g_answer_collect_ms_by_thread[MAX_N_THREAD] = {0.0};

static double   g_pipeline_acquire_ms_by_thread[MAX_N_THREAD] = {0.0};
static double   g_pipeline_inline_build_ms_by_thread[MAX_N_THREAD] = {0.0};
static uint64_t g_pipeline_pop_hits_by_thread[MAX_N_THREAD] = {0};
static uint64_t g_pipeline_pop_misses_by_thread[MAX_N_THREAD] = {0};
static double   g_pipeline_producer_build_ms_by_producer[MAX_N_THREAD] = {0.0};
static uint64_t g_pipeline_producer_build_count_by_producer[MAX_N_THREAD] = {0};
static uint64_t g_last_rootnum_by_thread[MAX_N_THREAD][64] = {{0}};
static uint64_t g_last_offset_by_thread[MAX_N_THREAD][64] = {{0}};
static node_t g_last_start_root_by_thread[MAX_N_THREAD][64] = {{0}};
static uint64_t g_last_assign_iter_by_thread[MAX_N_THREAD][64] = {{0}};

extern ans_t result[N];
extern uint64_t cycle_ct[N];
extern uint64_t cycle_ct_dpu[NR_DPUS][NR_TASKLETS];   /* per-DPU per-tasklet cycle load (PERF) */
extern Graph *global_g;
double get_time_in_milliseconds(struct timeval *start, struct timeval *end);

// Async finish-poll backoff. -DOFIS_NO_POLL_SLEEP => busy-poll (no usleep).
#ifdef OFIS_NO_POLL_SLEEP
#define POLL_BACKOFF() ((void)0)
#else
#define POLL_BACKOFF() usleep(100)
#endif

typedef struct {
    const char *name;
    double sum_ms;
    double avg_ms;
    double max_ms;
    uint32_t max_tid;
} stage_stat_t;

static inline uint32_t popcount64(uint64_t v) {
    return (uint32_t)__builtin_popcountll(v);
}

#ifndef OFIS_WAIT_FINISH_TIMEOUT_MS
#define OFIS_WAIT_FINISH_TIMEOUT_MS 30000ULL
#endif

typedef enum {
    DPU_DBG_STAGE_INIT = 0,
    DPU_DBG_STAGE_WAIT_FINISH,
    DPU_DBG_STAGE_GOT_FINISH,
    DPU_DBG_STAGE_ASSIGN,
    DPU_DBG_STAGE_XFER,
    DPU_DBG_STAGE_STATE_UPDATE,
    DPU_DBG_STAGE_EXIT,
} dpu_dbg_stage_t;

typedef struct {
    uint64_t heartbeat_ms;
    uint64_t loop_iter;
    uint64_t running_mask;
    uint64_t finish_mask;
    uint64_t assigned_mask;
    uint32_t stage;
    uint32_t done;
} dpu_thread_dbg_t;

static dpu_thread_dbg_t g_dpu_dbg[MAX_N_THREAD];

static inline uint64_t dbg_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
}

static inline void dpu_dbg_store(
    uint32_t tid,
    dpu_dbg_stage_t stage,
    uint64_t loop_iter,
    uint64_t running_mask,
    uint64_t finish_mask,
    uint64_t assigned_mask
) {
    __atomic_store_n(&g_dpu_dbg[tid].loop_iter, loop_iter, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dpu_dbg[tid].running_mask, running_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dpu_dbg[tid].finish_mask, finish_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dpu_dbg[tid].assigned_mask, assigned_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dpu_dbg[tid].stage, (uint32_t)stage, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dpu_dbg[tid].heartbeat_ms, dbg_now_ms(), __ATOMIC_RELAXED);
}

static inline const char *dpu_dbg_stage_name(uint32_t stage) {
    switch (stage) {
        case DPU_DBG_STAGE_INIT: return "init";
        case DPU_DBG_STAGE_WAIT_FINISH: return "wait_finish";
        case DPU_DBG_STAGE_GOT_FINISH: return "got_finish";
        case DPU_DBG_STAGE_ASSIGN: return "assign";
        case DPU_DBG_STAGE_XFER: return "xfer";
        case DPU_DBG_STAGE_STATE_UPDATE: return "state_update";
        case DPU_DBG_STAGE_EXIT: return "exit";
        default: return "unknown";
    }
}

static void print_stage_report_line(FILE *fp, const stage_stat_t *s, double total_ms) {
    const double pct = (total_ms > 0.0) ? (100.0 * s->avg_ms / total_ms) : 0.0;
    printf("  %-16s avg=%10.3f ms (%6.2f%%), max-thread=%9.3f ms (T%u)\n",
           s->name, s->avg_ms, pct, s->max_ms, s->max_tid);
    if (fp) {
        fprintf(fp, "  %-16s avg=%10.3f ms (%6.2f%%), max-thread=%9.3f ms (T%u)\n",
                s->name, s->avg_ms, pct, s->max_ms, s->max_tid);
    }
}

static stage_stat_t build_stage_stat(const char *name, const double *arr) {
    stage_stat_t s = {.name = name, .sum_ms = 0.0, .avg_ms = 0.0, .max_ms = 0.0, .max_tid = 0};
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) {
        const double v = arr[tid];
        s.sum_ms += v;
        if (v > s.max_ms) {
            s.max_ms = v;
            s.max_tid = tid;
        }
    }
    s.avg_ms = (MAX_N_THREAD > 0) ? (s.sum_ms / (double)MAX_N_THREAD) : 0.0;
    return s;
}

static void report_scaling_bottleneck(FILE *fp) {
    stage_stat_t stages[] = {
        build_stage_stat("wait_finish", g_wait_ms_by_thread),
        build_stage_stat("assign_prepare", g_assign_ms_by_thread),
        build_stage_stat("xfer_root_num", g_t_rootnum_ms_by_thread),
        build_stage_stat("xfer_offset", g_t_offset_ms_by_thread),
        build_stage_stat("xfer_roots", g_t_roots_ms_by_thread),
        build_stage_stat("compact_build", g_compact_build_ms_by_thread),
        build_stage_stat("compact_xfer", g_compact_xfer_ms_by_thread),
        build_stage_stat("answer_collect", g_answer_collect_ms_by_thread),
        build_stage_stat("mux_to_host", g_mux_host_ms_by_thread),
        build_stage_stat("state_update", g_state_update_ms_by_thread),
        build_stage_stat("barrier_wait", g_barrier_wait_ms_by_thread),
        build_stage_stat("init_row_ptr", g_init_rowptr_ms_by_thread),
        build_stage_stat("init_col_idx", g_init_colidx_ms_by_thread),
    };
    const uint32_t nstages = sizeof(stages) / sizeof(stages[0]);

    double total_ms = 0.0;
    for (uint32_t i = 0; i < nstages; ++i) {
        total_ms += stages[i].avg_ms;
    }

    printf("\n=== OFIS Stage Bottleneck Report ===\n");
    if (fp) {
        fprintf(fp, "\n=== OFIS Stage Bottleneck Report ===\n");
    }
    for (uint32_t i = 0; i < nstages; ++i) {
        print_stage_report_line(fp, &stages[i], total_ms);
    }


#ifdef OFIS_HYBRID
    {
        const char *pn[3] = {"DPU", "IG", "RANK"};
        FILE *outs[2] = { stdout, fp };
        for (int oi = 0; oi < 2; ++oi) {
            FILE *o = outs[oi]; if (!o) continue;
            fprintf(o, "\n=== OFIS Per-Phase Breakdown (3-tier hybrid) ===\n");
            for (int ph = 0; ph < 3; ++ph) {
                double wsum=0, wmax=0, wmin=-1, isum=0, imax=0, xsum=0, bsum=0, csum=0,
                       ssum=0, smax=0, tsum=0, idsum=0, gsum=0;
                uint32_t nact=0, wmax_t=0, wmin_t=0, imax_t=0, smax_t=0;
                uint64_t phsum=0, pmsum=0, scnt=0;
                for (uint32_t t = 0; t < MAX_N_THREAD; ++t) {
                    const double w = g_hybrid_phase_wall_ms[ph][t];
                    const double idle = g_hybrid_phase_wait_ms[ph][t];
                    if (w <= 0.0 && idle <= 0.0) continue;
                    nact++; wsum += w; isum += idle;
                    xsum += g_hybrid_phase_xfer_ms[ph][t];
                    bsum += g_hybrid_phase_build_ms[ph][t];
                    csum += g_hybrid_phase_coll_ms[ph][t];
                    phsum += g_hybrid_phase_pophit[ph][t];
                    pmsum += g_hybrid_phase_popmiss[ph][t];
                    ssum += g_hybrid_phase_strag_sum_ms[ph][t];
                    scnt += g_hybrid_phase_strag_cnt[ph][t];
                    tsum += g_hybrid_phase_tail_ms[ph][t];
                    idsum += g_hybrid_phase_idle_dpu_ms[ph][t];
                    gsum += g_hybrid_trans_gap_ms[ph][t];
                    if (w > wmax) { wmax = w; wmax_t = t; }
                    if (wmin < 0 || w < wmin) { wmin = w; wmin_t = t; }
                    if (idle > imax) { imax = idle; imax_t = t; }
                    if (g_hybrid_phase_strag_max_ms[ph][t] > smax) { smax = g_hybrid_phase_strag_max_ms[ph][t]; smax_t = t; }
                }
                if (wmin < 0) wmin = 0;
                const double wavg = nact ? wsum/nact : 0, iavg = nact ? isum/nact : 0;
                const double xavg = nact ? xsum/nact : 0, cavg = nact ? csum/nact : 0;
                const double bavg = nact ? bsum/nact : 0;
                const double savg = nact ? ssum/nact : 0;
                const double sperround = scnt ? ssum/(double)scnt : 0;   /* avg straggler spread PER round */
                const double other = wavg - bavg - xavg - cavg - iavg;
                fprintf(o, "  ph.%-4s wall.avg=%9.1f | build=%8.1f xfer=%8.1f collect=%8.1f wait=%9.1f other=%8.1f"
                           " || wall.max=%9.1f(T%u) min=%9.1f(T%u) crossrank=%8.1f"
                           " | strag.sum=%8.1f perround=%.4f max=%8.1f(T%u) rounds=%lu\n",
                        pn[ph], wavg, bavg, xavg, cavg, iavg, other,
                        wmax, wmax_t, wmin, wmin_t, wmax - wmin, savg, sperround, smax, smax_t,
                        (unsigned long)scnt);
                { uint64_t tot = phsum + pmsum;
                  fprintf(o, "    pop.%-4s hits=%" PRIu64 " miss=%" PRIu64 " (%.1f%% miss -> inline build)\n",
                          pn[ph], phsum, pmsum, tot ? 100.0*(double)pmsum/(double)tot : 0.0); }
                const double tavg = nact ? tsum/nact : 0, idavg = nact ? idsum/nact : 0;
                const double gavg = nact ? gsum/nact : 0;
                fprintf(o, "    ph2.%-4s entry_gap=%8.1f | wait: steady=%9.1f drain_tail=%9.1f"
                           " (idle_dpu=%10.1f dpu-ms)\n",
                        pn[ph], gavg, iavg - tavg, tavg, idavg);
                fprintf(o, "    phT.%-4s wall[T0..]:", pn[ph]);
                for (uint32_t t = 0; t < MAX_N_THREAD; ++t)
                    if (g_hybrid_phase_wall_ms[ph][t] > 0.0) fprintf(o, " %.0f", g_hybrid_phase_wall_ms[ph][t]);
                fprintf(o, "  | wait[T0..]:");
                for (uint32_t t = 0; t < MAX_N_THREAD; ++t)
                    if (g_hybrid_phase_wall_ms[ph][t] > 0.0) fprintf(o, " %.0f", g_hybrid_phase_wait_ms[ph][t]);
                fprintf(o, "\n");
            }
            fprintf(o, "(build+xfer+collect+wait+other = wall.avg; crossrank = slowest-fastest rank-thread;"
                       " strag = per-round last-DPU minus first-DPU within the group = INTRA-group straggler)\n");
            fprintf(o, "================================================\n");

        }
    }
#endif
#ifdef DPU_TIME_PROFILE
    // Per-DPU kernel wall over the DPU-tier heavy phase: summary plus one "DPUK <gid> <ms> <launches>" line per DPU for straggler plots.
    {
        FILE *outs2[2] = { stdout, fp }; int nfp = (fp && fp != stdout) ? 2 : 1;
        for (int oi = 0; oi < nfp; ++oi) {
            FILE *o = outs2[oi]; if (!o) continue;
            double ksum=0, isum=0, xsum=0, kmx=0, imx=0, xmx=0; uint32_t nz=0;
            for (uint32_t d = 0; d < NR_DPUS; ++d) {
                if (g_dpu_launch_cnt[d] == 0) continue;
                nz++; ksum += g_dpu_kernel_ms[d]; isum += g_dpu_idle_ms[d]; xsum += g_dpu_xfer_ms[d];
                if (g_dpu_kernel_ms[d] > kmx) kmx = g_dpu_kernel_ms[d];
                if (g_dpu_idle_ms[d]   > imx) imx = g_dpu_idle_ms[d];
                if (g_dpu_xfer_ms[d]   > xmx) xmx = g_dpu_xfer_ms[d];
            }
            fprintf(o, "\n=== Per-DPU breakdown (all tiers DPU+IG+RANK, host-observed, ms) ===\n");
            fprintf(o, "  active_dpus=%u   (span = kernel + idle; xfer is the subgraph-transfer portion of idle)\n", nz);
            fprintf(o, "  kernel(busy) : mean=%.1f max=%.1f\n", (nz?ksum/nz:0), kmx);
            fprintf(o, "  idle(wait)   : mean=%.1f max=%.1f  (idle/span mean=%.1f%%)\n",
                    (nz?isum/nz:0), imx, (nz&&(ksum+isum)>0)? 100.0*isum/(ksum+isum):0);
            fprintf(o, "  xfer(of idle): mean=%.1f max=%.1f  (xfer/idle mean=%.1f%%)\n",
                    (nz?xsum/nz:0), xmx, (isum>0)? 100.0*xsum/isum:0);
            // one line per DPU: DPUK <gid> <kernel_ms> <idle_ms> <xfer_ms> <launches>
            for (uint32_t d = 0; d < NR_DPUS; ++d)
                if (g_dpu_launch_cnt[d])
                    fprintf(o, "DPUK %u %.3f %.3f %.3f %u\n",
                            d, g_dpu_kernel_ms[d], g_dpu_idle_ms[d], g_dpu_xfer_ms[d], g_dpu_launch_cnt[d]);
        }
    }
#endif
}



int OFIS_answers_collected_in_worker_threads(void) {
#if defined(OFIS_COLLECT_ON_FINAL_RETIRE) || defined(OFIS_COLLECT_PER_BATCH)
    return 1;
#else
    return 0;
#endif
}

double OFIS_get_answer_collect_max_ms(void) {
    double max_ms = 0.0;
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) {
        if (g_answer_collect_ms_by_thread[tid] > max_ms) {
            max_ms = g_answer_collect_ms_by_thread[tid];
        }
    }
    return max_ms;
}

// Mean over the ranks that actually collected (sum / active-ranks).
double OFIS_get_answer_collect_mean_ms(void) {
    double sum = 0.0; uint32_t n = 0;
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid)
        if (g_answer_collect_ms_by_thread[tid] > 0.0) { sum += g_answer_collect_ms_by_thread[tid]; n++; }
    return n ? sum / (double)n : 0.0;
}

// total collection work across all ranks (thread-ms)
double OFIS_get_answer_collect_sum_ms(void) {
    double sum = 0.0;
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) sum += g_answer_collect_ms_by_thread[tid];
    return sum;
}

// Number of result-collection transfers across all ranks. `calls` counts one per
// DPU-batch collected (sum of active DPUs over every collect) = total task batches
// under OFIS_COLLECT_PER_BATCH; `rounds` counts the rank-parallel push operations.
// Both are maintained unconditionally on the collect path (not PERF-gated).
uint64_t OFIS_get_answer_collect_calls(void) {
    uint64_t sum = 0;
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) sum += g_collect_calls_by_thread[tid];
    return sum;
}
uint64_t OFIS_get_answer_collect_rounds(void) {
    uint64_t sum = 0;
    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) sum += g_collect_rounds_by_thread[tid];
    return sum;
}

#ifdef OFIS_COLLECT_ON_FINAL_RETIRE
static void ofis_collect_results_for_mask(
    uint32_t thread_id,
    uint32_t rank_id,
    struct dpu_set_t rank,
    uint64_t collect_mask
) {
#ifdef OFIS_NO_FINAL_COLLECT
    (void)thread_id; (void)rank_id; (void)rank; (void)collect_mask;
    return;   /* skip result readback entirely -- measures E2E without collection */
#endif
    if (collect_mask == 0) {
        return;
    }

    struct timeval t0, t1;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    uint64_t max_root_count = 0;
    uint64_t active_count = 0;
    uint64_t **ans_bufs = NULL;
#ifdef PERF
    uint64_t **cycle_bufs = NULL;
#endif

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        if (root_count > max_root_count) max_root_count = root_count;
        active_count++;
    }
    if (active_count == 0 || max_root_count == 0) {
        return;
    }

    ans_bufs = calloc(64, sizeof(uint64_t *));
#ifdef PERF
    cycle_bufs = calloc(64, sizeof(uint64_t *));
#endif
    if (!ans_bufs
#ifdef PERF
        || !cycle_bufs
#endif
    ) {
        printf(ANSI_COLOR_RED "Error: answer collect buffer table alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }

#ifdef OFIS_SCALAR_ANS
    const uint64_t xfer_bytes = ALIGN8(sizeof(uint64_t)); /* one running total per DPU, in ans[0] */
#else
    const uint64_t xfer_bytes = ALIGN8(max_root_count * sizeof(uint64_t));
#endif
#ifdef PERF
    // cycle_ct is always per-root (OFIS_SCALAR_ANS collapses only `ans`), so move the full per-root region.
    const uint64_t cyc_xfer_bytes = ALIGN8(max_root_count * sizeof(uint64_t));
#endif
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        ans_bufs[each_dpu] = malloc(xfer_bytes);
#ifdef PERF
        cycle_bufs[each_dpu] = malloc(cyc_xfer_bytes);
#endif
        if (!ans_bufs[each_dpu]
#ifdef PERF
            || !cycle_bufs[each_dpu]
#endif
        ) {
            printf(ANSI_COLOR_RED "Error: answer collect buffer alloc failed for DPU %u\n" ANSI_COLOR_RESET, each_dpu);
            exit(1);
        }
    }

    gettimeofday(&t0, NULL);
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
        OFIS_prepare_xfer_dpu(dpu, collect_mask, ans_bufs[each_dpu]);
    }
    OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "ans", 0, xfer_bytes);

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
#ifdef OFIS_SCALAR_ANS
        // Scalar total in ans[0] attributed to the DPU's first root so main.c's per-root sum still totals correctly.
        result[global_g->roots[global_dpu][0]] = ans_bufs[each_dpu][0];
#else
        for (node_t k = 0; k < root_count; ++k) {
            node_t cur_root = global_g->roots[global_dpu][k];
            result[cur_root] = ans_bufs[each_dpu][k];
        }
#endif
    }

#ifdef PERF
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        OFIS_prepare_xfer_dpu(dpu, collect_mask, cycle_bufs[each_dpu]);
    }
    OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "cycle_ct", 0, cyc_xfer_bytes);

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        uint64_t _cyc_sum = 0;
        for (node_t k = 0; k < root_count; ++k) {
            node_t cur_root = global_g->roots[global_dpu][k];
            cycle_ct[cur_root] = cycle_bufs[each_dpu][k];
            _cyc_sum += cycle_bufs[each_dpu][k];
        }
        g_dpu_cycle_ct[rank_id][each_dpu] += _cyc_sum;   /* per-DPU cycle load */
    }
#endif
    gettimeofday(&t1, NULL);
    g_answer_collect_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    // one rank-parallel push of `active_count` scalar reads (8B each under SCALAR_ANS).
    g_collect_rounds_by_thread[thread_id] += 1;
    g_collect_calls_by_thread[thread_id]  += active_count;
    g_collect_bytes_by_thread[thread_id]  += active_count * xfer_bytes;

    for (uint32_t i = 0; i < 64; ++i) {
        free(ans_bufs[i]);
#ifdef PERF
        free(cycle_bufs[i]);
#endif
    }
    free(ans_bufs);
#ifdef PERF
    free(cycle_bufs);
#endif
}
#endif /* OFIS_COLLECT_ON_FINAL_RETIRE */

// IG/RANK variant: reads each DPU's whole ans[0..root_num) without touching the mux (caller opens the unit).
#if defined(OFIS_COLLECT_ON_FINAL_RETIRE) || (defined(OFIS_HYBRID) && !defined(OFIS_COLLECT_PER_BATCH))
static void collect_full_direct_nomux(
    uint32_t thread_id,
    uint32_t rank_id,
    struct dpu_set_t rank,
    uint64_t collect_mask
) {
#ifdef OFIS_NO_FINAL_COLLECT
    (void)thread_id; (void)rank_id; (void)rank; (void)collect_mask;
    return;   /* skip result readback entirely -- measures E2E without collection */
#endif
    if (collect_mask == 0) {
        return;
    }

    struct timeval t0, t1;
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    uint64_t max_root_count = 0;
    uint64_t active_count = 0;
    uint64_t **ans_bufs = NULL;
#ifdef PERF
    uint64_t **cycle_bufs = NULL;
#endif

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        if (root_count > max_root_count) max_root_count = root_count;
        active_count++;
    }
    if (active_count == 0 || max_root_count == 0) {
        return;
    }

    ans_bufs = calloc(64, sizeof(uint64_t *));
#ifdef PERF
    cycle_bufs = calloc(64, sizeof(uint64_t *));
#endif
    if (!ans_bufs
#ifdef PERF
        || !cycle_bufs
#endif
    ) {
        printf(ANSI_COLOR_RED "Error: answer collect buffer table alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }

#ifdef OFIS_SCALAR_ANS
    const uint64_t xfer_bytes = ALIGN8(sizeof(uint64_t)); /* one running total per DPU, in ans[0] */
#else
    const uint64_t xfer_bytes = ALIGN8(max_root_count * sizeof(uint64_t));
#endif
#ifdef PERF
    // cycle_ct is always per-root (OFIS_SCALAR_ANS collapses only `ans`), so move the full per-root region.
    const uint64_t cyc_xfer_bytes = ALIGN8(max_root_count * sizeof(uint64_t));
#endif
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        ans_bufs[each_dpu] = malloc(xfer_bytes);
#ifdef PERF
        cycle_bufs[each_dpu] = malloc(cyc_xfer_bytes);
#endif
        if (!ans_bufs[each_dpu]
#ifdef PERF
            || !cycle_bufs[each_dpu]
#endif
        ) {
            printf(ANSI_COLOR_RED "Error: answer collect buffer alloc failed for DPU %u\n" ANSI_COLOR_RESET, each_dpu);
            exit(1);
        }
    }

    gettimeofday(&t0, NULL);
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        OFIS_prepare_xfer_dpu(dpu, collect_mask, ans_bufs[each_dpu]);
    }
    OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "ans", 0, xfer_bytes);

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
#ifdef OFIS_SCALAR_ANS
        // Scalar total in ans[0] attributed to the DPU's first root so main.c's per-root sum still totals correctly.
        result[global_g->roots[global_dpu][0]] = ans_bufs[each_dpu][0];
#else
        for (node_t k = 0; k < root_count; ++k) {
            node_t cur_root = global_g->roots[global_dpu][k];
            result[cur_root] = ans_bufs[each_dpu][k];
        }
#endif
    }

#ifdef PERF
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        OFIS_prepare_xfer_dpu(dpu, collect_mask, cycle_bufs[each_dpu]);
    }
    OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "cycle_ct", 0, cyc_xfer_bytes);

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t root_count = global_g->root_num[global_dpu];
        if (root_count == 0) continue;
        uint64_t _cyc_sum = 0;
        for (node_t k = 0; k < root_count; ++k) {
            node_t cur_root = global_g->roots[global_dpu][k];
            cycle_ct[cur_root] = cycle_bufs[each_dpu][k];
            _cyc_sum += cycle_bufs[each_dpu][k];
        }
        g_dpu_cycle_ct[rank_id][each_dpu] += _cyc_sum;   /* per-DPU cycle load */
    }
#endif
    gettimeofday(&t1, NULL);
    g_answer_collect_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    // one rank-parallel push of `active_count` scalar reads (8B each under SCALAR_ANS).
    g_collect_rounds_by_thread[thread_id] += 1;
    g_collect_calls_by_thread[thread_id]  += active_count;
    g_collect_bytes_by_thread[thread_id]  += active_count * xfer_bytes;

    for (uint32_t i = 0; i < 64; ++i) {
        free(ans_bufs[i]);
#ifdef PERF
        free(cycle_bufs[i]);
#endif
    }
    free(ans_bufs);
#ifdef PERF
    free(cycle_bufs);
#endif
}
#endif /* collect_full_direct_nomux guard */

#ifdef OFIS_COLLECT_PER_BATCH
static void ofis_collect_batch_slice(
    uint32_t thread_id,
    uint32_t rank_id,
    struct dpu_set_t rank,
    uint64_t collect_mask,
    const uint64_t *last_offset,
    const uint64_t *last_root_num
) {
    if (collect_mask == 0) return;

    struct dpu_set_t dpu;
    uint32_t each_dpu;
    uint64_t max_cnt = 0;

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        if (last_root_num[each_dpu] > max_cnt) max_cnt = last_root_num[each_dpu];
    }
    if (max_cnt == 0) return;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint64_t cnt = last_root_num[each_dpu];
        if (cnt == 0) continue;
        const uint64_t off = last_offset[each_dpu];
        const uint32_t global_dpu = rank_id * 64 + each_dpu;

        const uint64_t one = 1ULL << each_dpu;

        // Stock dpu_copy_from mux is DPU-PAIR granular; use the OFIS per-DPU mux so neighbours keep running. Read only [off,off+cnt).
#ifdef OFIS_SCALAR_ANS
        const uint64_t rd_off = 0, bytes = ALIGN8(sizeof(uint64_t)); /* one running total in ans[0] */
        ofis_dpu_result_reserve(global_dpu, 1);
#else
        const uint64_t rd_off = off, bytes = ALIGN8(cnt * sizeof(uint64_t));
        ofis_dpu_result_reserve(global_dpu, off + cnt);
#endif
        OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);

        // Read race is handled by WHEN the caller reads (one round behind), not here.
        OFIS_prepare_xfer_dpu(dpu, one, &g_dpu_result_vals[global_dpu][rd_off]);
        OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "ans",
                               (uint32_t)(rd_off * sizeof(uint64_t)),
                               (size_t)bytes);
        g_collect_calls_by_thread[thread_id]++;
        g_collect_bytes_by_thread[thread_id] += bytes;
#ifdef PERF
        // Pull this batch's per-root cycle_ct [off,off+cnt) and add its sum to the DPU's running cycle load.
        {
            const uint64_t cyc_off = off, cyc_bytes = ALIGN8(cnt * sizeof(uint64_t));
            uint64_t *cc = (uint64_t *)malloc((size_t)cyc_bytes);
            if (cc) {
                OFIS_prepare_xfer_dpu(dpu, one, cc);
                OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "cycle_ct",
                                       (uint32_t)(cyc_off * sizeof(uint64_t)), (size_t)cyc_bytes);
                uint64_t s = 0; for (uint64_t k = 0; k < cnt; k++) s += cc[k];
                g_dpu_cycle_ct[rank_id][each_dpu] += s;
                free(cc);
            }
        }
#endif
    }
    gettimeofday(&t1, NULL);
    g_answer_collect_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    g_collect_rounds_by_thread[thread_id]++;
}

// IG/RANK slice read: like ofis_collect_batch_slice but does NOT touch the mux (unit must already be OPEN_TO_HOST).
static void collect_slice_nomux(
    uint32_t thread_id,
    uint32_t rank_id,
    struct dpu_set_t rank,
    uint64_t collect_mask,
    const uint64_t *last_offset,
    const uint64_t *last_root_num
) {
    if (collect_mask == 0) return;

    struct dpu_set_t dpu;
    uint32_t each_dpu;
    uint64_t max_cnt = 0;

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        if (last_root_num[each_dpu] > max_cnt) max_cnt = last_root_num[each_dpu];
    }
    if (max_cnt == 0) return;

    struct timeval t0, t1;
#ifdef GSDPU_BIN_THRESHOLD
    uint64_t cn[2] = {0, 0};
#endif
    gettimeofday(&t0, NULL);
    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((collect_mask & (1ULL << each_dpu)) == 0) continue;
        const uint64_t cnt = last_root_num[each_dpu];
        if (cnt == 0) continue;
        const uint64_t off = last_offset[each_dpu];
        const uint32_t global_dpu = rank_id * 64 + each_dpu;
        const uint64_t one = 1ULL << each_dpu;
#ifdef GSDPU_BIN_THRESHOLD
        cn[GSDPU_BIN(&global_g->roots[global_dpu][off])]++;
#endif

#ifdef OFIS_SCALAR_ANS
        const uint64_t rd_off = 0, bytes = ALIGN8(sizeof(uint64_t)); /* one running total in ans[0] */
        ofis_dpu_result_reserve(global_dpu, 1);
#else
        const uint64_t rd_off = off, bytes = ALIGN8(cnt * sizeof(uint64_t));
        ofis_dpu_result_reserve(global_dpu, off + cnt);
#endif
        OFIS_prepare_xfer_dpu(dpu, one, &g_dpu_result_vals[global_dpu][rd_off]);
        OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "ans",
                               (uint32_t)(rd_off * sizeof(uint64_t)),
                               (size_t)bytes);
        g_collect_calls_by_thread[thread_id]++;
        g_collect_bytes_by_thread[thread_id] += bytes;
    }
    gettimeofday(&t1, NULL);
    g_answer_collect_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    g_collect_rounds_by_thread[thread_id]++;
#ifdef GSDPU_BIN_THRESHOLD
    { const double cms = get_time_in_milliseconds(&t0, &t1);
      const uint64_t total = cn[0] + cn[1];
      for (int b = 0; b < 2 && total > 0; ++b) {
          g_gscollect_ms[b][thread_id] += cms * (double)cn[b] / (double)total;
          g_gscollect_nb[b][thread_id] += cn[b];
      } }
#endif
}

static double   g_collect_finalize_ms   = 0.0;
static uint64_t g_collect_finalize_vals = 0;

static void ofis_collect_finalize(void) {
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    uint64_t mapped = 0;
    for (uint32_t gd = 0; gd < NR_DPUS; gd++) {
        uint64_t *vals = g_dpu_result_vals[gd];
        const uint64_t rn_all = global_g->root_num[gd];
        if (!vals) continue;
        const node_t *roots = global_g->roots[gd];
        const uint64_t rn = rn_all;
#ifdef OFIS_SCALAR_ANS
        result[roots[0]] = vals[0];   /* single running total per DPU */
        mapped += 1;
#else
        for (uint64_t i = 0; i < rn; i++) {
            result[roots[i]] = vals[i];
        }
        mapped += rn;
#endif
        free(g_dpu_result_vals[gd]);
        g_dpu_result_vals[gd] = NULL;
        g_dpu_result_cap[gd]  = 0;
    }
    gettimeofday(&t1, NULL);
    g_collect_finalize_ms   = get_time_in_milliseconds(&t0, &t1);
    g_collect_finalize_vals = mapped;
    printf("[OFIS] deferred collect map+merge: %.3f ms (%" PRIu64 " values)\n",
           g_collect_finalize_ms, g_collect_finalize_vals);
}

// MUX-race fix: re-read the settled `ans` once per rank after all DPUs STOP.
static double g_collect_reread_ms = 0.0;
double get_collect_reread_ms(void) { return g_collect_reread_ms; }

static void collect_settled_reread(struct dpu_set_t set) {
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    for (uint32_t r = 0; r < MAX_N_RANK; ++r) {
        struct dpu_set_t rank = OFIS_get_rank(set, r);

        uint64_t all_mask = 0;
        uint64_t max_cnt  = 0;
        DPU_FOREACH(rank, dpu, each_dpu) {
            const uint32_t gd = r * 64 + each_dpu;
            const uint64_t cnt = global_g->root_num[gd];
            if (cnt == 0) continue;
            if (cnt > max_cnt) max_cnt = cnt;
            all_mask |= (1ULL << each_dpu);
        }
        if (max_cnt == 0 || all_mask == 0) continue;

        DPU_FOREACH(rank, dpu, each_dpu) {
            if ((all_mask & (1ULL << each_dpu)) == 0) continue;
            const uint32_t gd = r * 64 + each_dpu;
            ofis_dpu_result_reserve(gd, max_cnt);
            OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
            OFIS_prepare_xfer_dpu(dpu, all_mask, &g_dpu_result_vals[gd][0]);
        }
        OFIS_push_xfer(rank, DPU_XFER_FROM_DPU, "ans", 0,
#ifdef OFIS_SCALAR_ANS
                               (size_t)ALIGN8(sizeof(uint64_t))); /* one running total */
#else
                               (size_t)ALIGN8(max_cnt * sizeof(uint64_t)));
#endif
    }
    gettimeofday(&t1, NULL);
    g_collect_reread_ms = get_time_in_milliseconds(&t0, &t1);
}
#endif /* OFIS_COLLECT_PER_BATCH */

// Per-batch root cap and roots staging buffer size; override with -DROOTS_PER_DPU=N.
// Size default only (no engine behavior): with BATCH_DEG_BUDGET set, the cap defaults
// to it so it is never smaller than a budget batch; pass -DROOTS_PER_DPU=N to size it
// independently. BATCH_DEG_BUDGET sizes actual batches only under BATCH_TIER_BY_COUNT
// or OFIS_PREBUILD_ALL; on the plain pipeline it affects only this cap.
#ifndef ROOTS_PER_DPU
#ifdef BATCH_DEG_BUDGET
#define ROOTS_PER_DPU BATCH_DEG_BUDGET
#else
#define ROOTS_PER_DPU 256
#endif
#endif
#ifndef HEAVY_NODE_DIVISOR
#define HEAVY_NODE_DIVISOR 16
#endif

static node_t *g_all_roots = NULL;
static uint64_t
g_total_roots = 0;
static uint32_t g_auto_batch_size = DPU_ROOT_NUM;
#ifdef OFIS_HYBRID
static node_t   g_split           = 0;   /* cut 1: first root with deg < GRAN_SPLIT (DPU|IG) */
static node_t   g_split2          = 0;   /* cut 2: first root with deg < GRAN_RANK  (IG|RANK) */
static uint64_t g_split_batch_idx  = 0;  /* first IG-region batch index  */
static uint64_t g_split2_batch_idx = 0;  /* first RANK-region batch index */
static uint64_t g_prebuilt_next_heavy = 0;
static uint64_t g_prebuilt_next_light = 0;
static uint64_t g_prebuilt_next_rank  = 0;
static __thread int g_hybrid_pull_tier = 0;   /* 0=heavy/DPU, 1=medium/IG, 2=light/RANK */
#endif

static node_t *g_root_buf_storage[NR_DPUS] = {0};

uint32_t p_count; // to count nr_part sent to DPUs

Graph *global_g;
double workload[N];
static int g_use_batch_compact = 0;

// batch_compact_build hot-path knobs (default ON): fast bitmap clear and radix-sort node ids.
#ifndef COMPACT_FAST_CLEAR
#define COMPACT_FAST_CLEAR 1
#endif
#ifndef COMPACT_RADIX_SORT
#define COMPACT_RADIX_SORT 1
#endif

typedef struct batch_compact_ctx_t {
    uint32_t *base_bitmap;
    uint32_t *involve_bitmap;
    node_t *base_nodes;
    node_t *involved_nodes;
    edge_ptr *local_row_ptr;
    node_t *local_col_idx;
    node_t *local_roots;
    uint64_t local_roots_capacity;
    node_t *global_to_local;
    node_t *mapped_nodes;
    uint32_t n_words;
#if COMPACT_RADIX_SORT
    node_t *sort_scratch;      /* ping-pong buffer for the radix passes */
    uint32_t radix_bytes;      /* how many 8-bit digits the ids actually need */
#endif
} batch_compact_ctx_t;

typedef struct compact_payload_t {
    int valid;
    uint64_t root_num;
    uint64_t offset;
    uint64_t row_bytes;
    uint64_t col_bytes;
    node_t *roots;
    uint64_t roots_capacity;
    edge_ptr *row_ptr;
    node_t *col_idx;
} compact_payload_t;

static inline int bc_check_bit(const uint32_t *bitmap, node_t n) {
    return (bitmap[n >> 5] & (1U << (n & 31))) != 0;
}

static inline int bc_set_bit(uint32_t *bitmap, node_t n) {
    uint32_t mask = (1U << (n & 31));
    uint32_t *word = &bitmap[n >> 5];
    const int already = ((*word & mask) != 0);
    *word |= mask;
    return !already;
}

static inline void bc_clear_bit(uint32_t *bitmap, node_t n) {
    bitmap[n >> 5] &= ~(1U << (n & 31));
}

static int bc_node_t_cmp(const void *a, const void *b) {
    node_t x = *(const node_t *)a;
    node_t y = *(const node_t *)b;
    return (x > y) - (x < y);
}

// LSD radix sort over `bytes` 8-bit digits; scratch holds n nodes, result in `a`.
static void bc_radix_sort(node_t *a, node_t *scratch, uint64_t n, uint32_t bytes) {
    if (n < 2) return;
    node_t *src = a, *dst = scratch;
    for (uint32_t b = 0; b < bytes; b++) {
        uint32_t count[257] = {0};
        const uint32_t shift = b * 8;
        for (uint64_t i = 0; i < n; i++) {
            count[((src[i] >> shift) & 0xFFu) + 1]++;
        }
        uint32_t nonzero = 0;
        for (uint32_t d = 1; d <= 256; d++) {
            if (count[d]) nonzero++;
        }
        if (nonzero <= 1) continue;
        for (uint32_t d = 1; d <= 256; d++) {
            count[d] += count[d - 1];
        }
        for (uint64_t i = 0; i < n; i++) {
            dst[count[(src[i] >> shift) & 0xFFu]++] = src[i];
        }
        node_t *t = src; src = dst; dst = t;
    }
    if (src != a) {
        memcpy(a, src, (size_t)n * sizeof(node_t));
    }
}

static void batch_compact_ctx_ensure_root_capacity(batch_compact_ctx_t *ctx, uint64_t root_count) {
    if (root_count <= ctx->local_roots_capacity) {
        return;
    }

    node_t *new_roots = realloc(ctx->local_roots, (size_t)root_count * sizeof(node_t));
    if (!new_roots) {
        printf(ANSI_COLOR_RED "Error: compact ctx root buffer grow failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    ctx->local_roots = new_roots;
    ctx->local_roots_capacity = root_count;
}

static void compact_payload_ensure_root_capacity(compact_payload_t *p, uint64_t root_count) {
    if (root_count <= p->roots_capacity) {
        return;
    }

    const uint64_t bytes = ALIGN8(root_count * sizeof(node_t));
    node_t *new_roots = realloc(p->roots, (size_t)bytes);
    if (!new_roots) {
        printf(ANSI_COLOR_RED "Error: compact payload root buffer grow failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    p->roots = new_roots;
    p->roots_capacity = root_count;
}

static void batch_compact_ctx_init(batch_compact_ctx_t *ctx, node_t n) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->n_words = (uint32_t)((n + 31) >> 5);
    ctx->base_bitmap = calloc(ctx->n_words, sizeof(uint32_t));
    ctx->involve_bitmap = calloc(ctx->n_words, sizeof(uint32_t));
    ctx->base_nodes = malloc((size_t)DPU_N * sizeof(node_t));
    ctx->involved_nodes = malloc((size_t)DPU_N * sizeof(node_t));
    ctx->local_row_ptr = malloc((size_t)DPU_N * sizeof(edge_ptr));
    ctx->local_col_idx = malloc((size_t)DPU_M * sizeof(node_t));
    ctx->local_roots = NULL;
    ctx->local_roots_capacity = 0;
    ctx->global_to_local = malloc((size_t)n * sizeof(node_t));
    ctx->mapped_nodes = malloc((size_t)DPU_N * sizeof(node_t));
#if COMPACT_RADIX_SORT
    ctx->sort_scratch = malloc((size_t)DPU_N * sizeof(node_t));
    // Test the shift bound before shifting (u32 >> 32 is UB / infinite loop).
    ctx->radix_bytes = 1;
    {
        const node_t hi = n ? (node_t)(n - 1) : 0;
        while (ctx->radix_bytes < 4 && (hi >> (8 * ctx->radix_bytes)) != 0) {
            ctx->radix_bytes++;
        }
    }
    if (!ctx->sort_scratch) {
        printf(ANSI_COLOR_RED "Error: radix scratch alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
#endif
    if (!ctx->base_bitmap || !ctx->involve_bitmap || !ctx->base_nodes || !ctx->involved_nodes ||
        !ctx->local_row_ptr || !ctx->local_col_idx ||
        !ctx->global_to_local || !ctx->mapped_nodes) {
        printf(ANSI_COLOR_RED "Error: batch compact ctx alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    for (node_t u = 0; u < n; u++) {
        ctx->global_to_local[u] = INVALID_NODE;
    }
}

static void batch_compact_ctx_free(batch_compact_ctx_t *ctx) {
    free(ctx->base_bitmap);
    free(ctx->involve_bitmap);
    free(ctx->base_nodes);
    free(ctx->involved_nodes);
    free(ctx->local_row_ptr);
    free(ctx->local_col_idx);
    free(ctx->local_roots);
    free(ctx->global_to_local);
    free(ctx->mapped_nodes);
#if COMPACT_RADIX_SORT
    free(ctx->sort_scratch);
#endif
    memset(ctx, 0, sizeof(*ctx));
}

static void compact_payload_reset(compact_payload_t *p) {
    if (p->roots) free(p->roots);
    if (p->row_ptr) free(p->row_ptr);
    if (p->col_idx) free(p->col_idx);
    memset(p, 0, sizeof(*p));
}

static void compact_payloads_init(compact_payload_t *payloads, uint32_t count) {
    const size_t row_bytes = (size_t)ALIGN8((uint64_t)DPU_N * sizeof(edge_ptr));
    const size_t col_bytes = (size_t)ALIGN8((uint64_t)DPU_M * sizeof(node_t));
    for (uint32_t i = 0; i < count; i++) {
        payloads[i].roots = NULL;
        payloads[i].roots_capacity = 0;
        payloads[i].row_ptr = malloc(row_bytes);
        payloads[i].col_idx = malloc(col_bytes == 0 ? 8 : col_bytes);
        if (!payloads[i].row_ptr || !payloads[i].col_idx) {
            printf(ANSI_COLOR_RED "Error: compact payload alloc failed\n" ANSI_COLOR_RESET);
            exit(1);
        }
        payloads[i].valid = 0;
        payloads[i].root_num = 0;
        payloads[i].offset = 0;
        payloads[i].row_bytes = 0;
        payloads[i].col_bytes = 0;
    }
}


static void batch_compact_build(
    batch_compact_ctx_t *ctx,
    const node_t *roots_global,
    uint64_t root_num,
    uint64_t *local_n_out,
    uint64_t *local_m_out
) {
    batch_compact_ctx_ensure_root_capacity(ctx, root_num);
#if !COMPACT_FAST_CLEAR
    memset(ctx->base_bitmap, 0, (size_t)ctx->n_words * sizeof(uint32_t));
    memset(ctx->involve_bitmap, 0, (size_t)ctx->n_words * sizeof(uint32_t));
#else
    // Bitmaps stay all-zero: each build clears exactly the bits it set (tail).
#endif

    uint64_t base_count = 0;
    for (uint64_t i = 0; i < root_num; i++) {
        node_t root = roots_global[i];
        if (bc_set_bit(ctx->base_bitmap, root)) {
            if (base_count >= DPU_N) {
                printf(ANSI_COLOR_RED "Error: base node overflow\n" ANSI_COLOR_RESET);
                exit(1);
            }
            ctx->base_nodes[base_count++] = root;
        }
        for (edge_ptr j = global_g->row_ptr[root]; j < global_g->row_ptr[root + 1]; j++) {
            node_t nbr = global_g->col_idx[j];
            if (bc_set_bit(ctx->base_bitmap, nbr)) {
                if (base_count >= DPU_N) {
                    printf(ANSI_COLOR_RED "Error: base node overflow\n" ANSI_COLOR_RESET);
                    exit(1);
                }
                ctx->base_nodes[base_count++] = nbr;
            }
        }
    }

    uint64_t local_n = 0;
    for (uint64_t i = 0; i < base_count; i++) {
        node_t u = ctx->base_nodes[i];
        if (bc_set_bit(ctx->involve_bitmap, u)) {
            if (local_n >= DPU_N - 1) {
                printf(ANSI_COLOR_RED "Error: compact local_n overflow\n" ANSI_COLOR_RESET);
                exit(1);
            }
            ctx->involved_nodes[local_n++] = u;
        }
        for (edge_ptr j = global_g->row_ptr[u]; j < global_g->row_ptr[u + 1]; j++) {
            node_t v = global_g->col_idx[j];
            if (bc_set_bit(ctx->involve_bitmap, v)) {
                if (local_n >= DPU_N - 1) {
                    printf(ANSI_COLOR_RED "Error: compact local_n overflow\n" ANSI_COLOR_RESET);
                    exit(1);
                }
                ctx->involved_nodes[local_n++] = v;
            }
        }
    }

#if COMPACT_RADIX_SORT
    bc_radix_sort(ctx->involved_nodes, ctx->sort_scratch, local_n, ctx->radix_bytes);
#else
    qsort(ctx->involved_nodes, local_n, sizeof(node_t), bc_node_t_cmp);
#endif

    for (uint64_t i = 0; i < local_n; i++) {
        const node_t u = ctx->involved_nodes[i];
        ctx->global_to_local[u] = (node_t)i;
        ctx->mapped_nodes[i] = u;
    }

    uint64_t local_m = 0;
    for (uint64_t i = 0; i < local_n; i++) {
        node_t u = ctx->involved_nodes[i];
        ctx->local_row_ptr[i] = (edge_ptr)local_m;
        if (bc_check_bit(ctx->base_bitmap, u)) {
            for (edge_ptr j = global_g->row_ptr[u]; j < global_g->row_ptr[u + 1]; j++) {
                node_t v = global_g->col_idx[j];
                node_t mapped = ctx->global_to_local[v];
                if (mapped == INVALID_NODE) {
                    printf(ANSI_COLOR_RED "Error: compact mapping failed for node %u\n" ANSI_COLOR_RESET, v);
                    exit(1);
                }
                if (local_m >= DPU_M) {
                    printf(ANSI_COLOR_RED "Error: compact local_m overflow\n" ANSI_COLOR_RESET);
                    exit(1);
                }
                ctx->local_col_idx[local_m++] = mapped;
            }
        }
    }
    ctx->local_row_ptr[local_n] = (edge_ptr)local_m;

    for (uint64_t i = 0; i < root_num; i++) {
        node_t mapped = ctx->global_to_local[roots_global[i]];
        if (mapped == INVALID_NODE) {
            printf(ANSI_COLOR_RED "Error: compact mapping failed for root %u\n" ANSI_COLOR_RESET, roots_global[i]);
            exit(1);
        }
        ctx->local_roots[i] = mapped;
    }

    for (uint64_t i = 0; i < local_n; i++) {
        ctx->global_to_local[ctx->mapped_nodes[i]] = INVALID_NODE;
    }

#if COMPACT_FAST_CLEAR
    // Restore both bitmaps to all-zero by clearing exactly the bits this build set.
    for (uint64_t i = 0; i < base_count; i++) {
        bc_clear_bit(ctx->base_bitmap, ctx->base_nodes[i]);
    }
    for (uint64_t i = 0; i < local_n; i++) {
        bc_clear_bit(ctx->involve_bitmap, ctx->involved_nodes[i]);
    }
#endif

    *local_n_out = local_n;
    *local_m_out = local_m;
    __atomic_add_fetch(&g_bcb_total_edges, local_m, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_bcb_total_nodes, local_n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_bcb_calls, 1, __ATOMIC_RELAXED);

}

static uint32_t find_compact_safe_prefix_count(
    node_t start_node,
    uint32_t max_batch_size,
    uint32_t *bitmap,
    uint8_t *touched_marks,
    uint32_t *touched_words,
    uint32_t *touched_count,
    edge_ptr *local_m_out);

// Compact prefetch pipeline: producers CAS-reserve root ranges, build payloads, push to a bounded queue; workers try-pop or build inline.
#ifndef OFIS_COMPACT_PIPELINE_QUEUE_DEPTH
#define OFIS_COMPACT_PIPELINE_QUEUE_DEPTH 128
#endif
#ifndef OFIS_COMPACT_PIPELINE_PRODUCERS
#define OFIS_COMPACT_PIPELINE_PRODUCERS 4
#endif

typedef struct compact_pipeline_payload_t {
    int valid;
    uint64_t start_node;     /* first global root in [start_node, start_node+root_num) */
    uint64_t root_num;
    uint64_t row_bytes;      /* ALIGN8((local_n + 1) * sizeof(edge_ptr))             */
    uint64_t col_bytes;      /* ALIGN8(local_m * sizeof(node_t))                     */
    uint64_t roots_capacity; /* current allocation count of `roots`                  */
    node_t *roots;           /* LOCAL (renumbered) roots — destined for DPU MRAM     */
    edge_ptr *row_ptr;       /* local row_ptr                                        */
    node_t *col_idx;         /* local col_idx                                        */
} compact_pipeline_payload_t;

typedef struct compact_pipeline_queue_t {
    compact_pipeline_payload_t **ring;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int shutdown;            /* set on teardown                                      */
} compact_pipeline_queue_t;

typedef struct compact_pipeline_pool_t {
    compact_pipeline_payload_t *slots;       /* array of payload structs            */
    compact_pipeline_payload_t **free_stack; /* free list                            */
    uint32_t capacity;
    uint32_t free_count;
    pthread_mutex_t lock;
    pthread_cond_t available;
} compact_pipeline_pool_t;

typedef struct compact_producer_state_t {
    batch_compact_ctx_t ctx;
    uint32_t *prefix_bitmap;
    uint8_t  *prefix_touched_marks;
    uint32_t *prefix_touched_words;
    uint32_t  prefix_touched_count;
    node_t   *roots_global_scratch;  /* DPU_ROOT_NUM contiguous IDs                  */
} compact_producer_state_t;

typedef struct compact_pipeline_t {
    int enabled;
    int initialized;
    compact_pipeline_queue_t queue;
    compact_pipeline_pool_t  pool;
    pthread_t *producer_threads;
    compact_producer_state_t *producer_states;
    uint32_t n_producers;
    int producer_done;  /* set when p_count has reached g_total_roots */
    uint32_t inflight;  /* roots reserved on p_count but not yet published */
#ifdef OFIS_HYBRID
    compact_pipeline_queue_t queue_light;
    compact_pipeline_pool_t  pool_light;
    uint32_t inflight_light;
    compact_pipeline_queue_t queue_rank;
    compact_pipeline_pool_t  pool_rank;
    uint32_t inflight_rank;
#endif
} compact_pipeline_t;

static compact_pipeline_t g_pipeline = { 0 };

static void compact_pipeline_payload_init(compact_pipeline_payload_t *p) {
    const size_t row_bytes = (size_t)ALIGN8((uint64_t)DPU_N * sizeof(edge_ptr));
    const size_t col_bytes = (size_t)ALIGN8((uint64_t)DPU_M * sizeof(node_t));
    p->valid = 0;
    p->start_node = 0;
    p->root_num = 0;
    p->row_bytes = 0;
    p->col_bytes = 0;
    p->roots = NULL;
    p->roots_capacity = 0;
    p->row_ptr = malloc(row_bytes);
    p->col_idx = malloc(col_bytes == 0 ? 8 : col_bytes);
    if (!p->row_ptr || !p->col_idx) {
        printf(ANSI_COLOR_RED "Error: pipeline payload alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
}

static void compact_pipeline_payload_free(compact_pipeline_payload_t *p) {
    free(p->roots);
    free(p->row_ptr);
    free(p->col_idx);
    memset(p, 0, sizeof(*p));
}

static void compact_pipeline_payload_ensure_root_capacity(compact_pipeline_payload_t *p, uint64_t root_count) {
    if (root_count <= p->roots_capacity) return;
    const uint64_t bytes = ALIGN8(root_count * sizeof(node_t));
    node_t *new_roots = realloc(p->roots, (size_t)bytes);
    if (!new_roots) {
        printf(ANSI_COLOR_RED "Error: pipeline payload root grow failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    p->roots = new_roots;
    p->roots_capacity = root_count;
}

static void compact_pipeline_queue_init(compact_pipeline_queue_t *q, uint32_t capacity) {
    q->ring = calloc(capacity, sizeof(compact_pipeline_payload_t *));
    if (!q->ring) {
        printf(ANSI_COLOR_RED "Error: pipeline queue alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    q->capacity = capacity;
    q->head = q->tail = q->count = 0;
    q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void compact_pipeline_queue_destroy(compact_pipeline_queue_t *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    free(q->ring);
    memset(q, 0, sizeof(*q));
}

// Returns 0 (drop) only on shutdown; a drop loses the caller's reserved roots.
static int compact_pipeline_queue_push(compact_pipeline_queue_t *q, compact_pipeline_payload_t *pl) {
    pthread_mutex_lock(&q->lock);
    while (q->count == q->capacity && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }
    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    q->ring[q->tail] = pl;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

static compact_pipeline_payload_t *compact_pipeline_queue_try_pop(compact_pipeline_queue_t *q) {
    compact_pipeline_payload_t *pl = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->count > 0) {
        pl = q->ring[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->lock);
    return pl;
}

// Blocking pop for end-of-work: NULL only when queue empty AND no producer still owes a batch.
static compact_pipeline_payload_t *compact_pipeline_queue_pop_drain_on(
        compact_pipeline_queue_t *q, uint32_t *inflight_ptr) {
    compact_pipeline_payload_t *pl = NULL;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        if (q->count > 0) {
            pl = q->ring[q->head];
            q->head = (q->head + 1) % q->capacity;
            q->count--;
            pthread_cond_signal(&q->not_full);
            break;
        }
        if (__atomic_load_n(inflight_ptr, __ATOMIC_ACQUIRE) == 0 || q->shutdown) {
            break;   /* nothing queued, nobody building: done */
        }
        pthread_cond_wait(&q->not_empty, &q->lock);
    }
    pthread_mutex_unlock(&q->lock);
    return pl;
}

static void compact_pipeline_queue_shutdown(compact_pipeline_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

static void compact_pipeline_pool_init(compact_pipeline_pool_t *pool, uint32_t capacity) {
    pool->slots = calloc(capacity, sizeof(compact_pipeline_payload_t));
    pool->free_stack = calloc(capacity, sizeof(compact_pipeline_payload_t *));
    if (!pool->slots || !pool->free_stack) {
        printf(ANSI_COLOR_RED "Error: pipeline pool alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    pool->capacity = capacity;
    pool->free_count = capacity;
    for (uint32_t i = 0; i < capacity; i++) {
        compact_pipeline_payload_init(&pool->slots[i]);
        pool->free_stack[i] = &pool->slots[i];
    }
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->available, NULL);
}

static void compact_pipeline_pool_destroy(compact_pipeline_pool_t *pool) {
    for (uint32_t i = 0; i < pool->capacity; i++) {
        compact_pipeline_payload_free(&pool->slots[i]);
    }
    free(pool->slots);
    free(pool->free_stack);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->available);
    memset(pool, 0, sizeof(*pool));
}

static compact_pipeline_payload_t *compact_pipeline_pool_acquire(compact_pipeline_pool_t *pool) {
    pthread_mutex_lock(&pool->lock);
    while (pool->free_count == 0 && !g_pipeline.queue.shutdown) {
        pthread_cond_wait(&pool->available, &pool->lock);
    }
    if (g_pipeline.queue.shutdown && pool->free_count == 0) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }
    compact_pipeline_payload_t *pl = pool->free_stack[--pool->free_count];
    pthread_mutex_unlock(&pool->lock);
    return pl;
}

static void compact_pipeline_pool_release(compact_pipeline_pool_t *pool, compact_pipeline_payload_t *pl) {
    pthread_mutex_lock(&pool->lock);
    pl->valid = 0;
    pool->free_stack[pool->free_count++] = pl;
    pthread_cond_signal(&pool->available);
    pthread_mutex_unlock(&pool->lock);
}

static void compact_producer_state_init(compact_producer_state_t *s, node_t graph_n) {
    batch_compact_ctx_init(&s->ctx, graph_n);
    const uint32_t n_words = (graph_n + 31U) >> 5;
    s->prefix_bitmap = calloc((size_t)n_words, sizeof(uint32_t));
    s->prefix_touched_marks = calloc((size_t)n_words, sizeof(uint8_t));
    s->prefix_touched_words = malloc((size_t)n_words * sizeof(uint32_t));
    s->prefix_touched_count = 0;
    s->roots_global_scratch = malloc((size_t)DPU_ROOT_NUM * sizeof(node_t));
    if (!s->prefix_bitmap || !s->prefix_touched_marks || !s->prefix_touched_words ||
        !s->roots_global_scratch) {
        printf(ANSI_COLOR_RED "Error: producer state alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
}

static void compact_producer_state_free(compact_producer_state_t *s) {
    batch_compact_ctx_free(&s->ctx);
    free(s->prefix_bitmap);
    free(s->prefix_touched_marks);
    free(s->prefix_touched_words);
    free(s->roots_global_scratch);
    memset(s, 0, sizeof(*s));
}

#ifndef GRAN_IG_BATCH_N
#define GRAN_IG_BATCH_N 32
#endif
#ifndef GRAN_RANK_BATCH_N
#define GRAN_RANK_BATCH_N 128
#endif
#ifdef BATCH_DEG_BUDGET
static uint32_t batch_count_by_deg_budget(node_t start, uint64_t remain);  /* defined below */
#endif
static void *compact_producer_thread_fn(void *arg) {
    compact_producer_state_t *st = (compact_producer_state_t *)arg;

    // Producer CPU pinning: 0=none, 1=CPU MAX_N_THREAD+i, 2=CPU MAX_N_THREAD*2+i.
#ifndef PRODUCER_PIN
#define PRODUCER_PIN 1
#endif
    const int producer_idx = (int)(st - g_pipeline.producer_states);
#if PRODUCER_PIN != 0
    {
        const long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
#if PRODUCER_PIN == 2
        int target_cpu = (int)(MAX_N_THREAD * 2) + producer_idx;
#else
        int target_cpu = (int)MAX_N_THREAD + producer_idx;
#endif
        if (nprocs > 0 && target_cpu < (int)nprocs) {
            cpu_set_t cs;
            CPU_ZERO(&cs);
            CPU_SET(target_cpu, &cs);
            pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
        }
    }
#endif

    while (!__atomic_load_n(&g_pipeline.queue.shutdown, __ATOMIC_ACQUIRE)) {
        uint32_t start = 0;
        uint32_t cnt = 0;
#ifdef OFIS_HYBRID
        int tier = 0;   /* 0 heavy/DPU, 1 medium/IG, 2 light/RANK */
#endif
        for (;;) {
            uint32_t snapshot = __atomic_load_n(&p_count, __ATOMIC_ACQUIRE);
            if (snapshot >= g_total_roots) {
                __atomic_store_n(&g_pipeline.producer_done, 1, __ATOMIC_RELEASE);
                pthread_mutex_lock(&g_pipeline.queue.lock);
                pthread_cond_broadcast(&g_pipeline.queue.not_empty);
                pthread_mutex_unlock(&g_pipeline.queue.lock);
#ifdef OFIS_HYBRID
                pthread_mutex_lock(&g_pipeline.queue_light.lock);
                pthread_cond_broadcast(&g_pipeline.queue_light.not_empty);
                pthread_mutex_unlock(&g_pipeline.queue_light.lock);
                pthread_mutex_lock(&g_pipeline.queue_rank.lock);
                pthread_cond_broadcast(&g_pipeline.queue_rank.not_empty);
                pthread_mutex_unlock(&g_pipeline.queue_rank.lock);
#endif
                return NULL;
            }
            uint64_t remain = g_total_roots - snapshot;
            node_t cur_node = snapshot;
            edge_ptr degree = global_g->row_ptr[cur_node + 1] - global_g->row_ptr[cur_node];
#if defined(OFIS_HYBRID) && defined(BATCH_TIER_BY_COUNT)
            uint32_t want;
            if (degree >= BATCH_HEAVY_THRESHOLD) want = 1;
            else want = batch_count_by_deg_budget(cur_node, remain);
#else
            uint32_t current_batch_size = g_auto_batch_size;
            if (degree >= BATCH_HEAVY_THRESHOLD) {
                current_batch_size = g_auto_batch_size / HEAVY_NODE_DIVISOR;
                if (current_batch_size == 0) current_batch_size = 1;
            }
            uint32_t want = (remain > current_batch_size) ? current_batch_size : (uint32_t)remain;
#endif
            edge_ptr est_m = 0;
            uint32_t safe = find_compact_safe_prefix_count(
                cur_node, want,
                st->prefix_bitmap,
                st->prefix_touched_marks,
                st->prefix_touched_words,
                &st->prefix_touched_count,
                &est_m);
            if (safe == 0) {
                printf(ANSI_COLOR_RED
                       "Error[producer]: root %u alone exceeds compact DPU_M bound\n"
                       ANSI_COLOR_RESET, cur_node);
                exit(1);
            }
            uint32_t expected = snapshot;
            uint32_t desired = snapshot + safe;
            if (__atomic_compare_exchange_n(&p_count, &expected, desired,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                // inflight++ before the batch is visible so p_count>=total never appears with it unaccounted for.
#ifdef OFIS_HYBRID
                tier = (snapshot >= (uint32_t)g_split2) ? 2
                     : (snapshot >= (uint32_t)g_split)  ? 1 : 0;
#ifndef OFIS_NO_DROP_FIX
                __atomic_fetch_add(tier == 2 ? &g_pipeline.inflight_rank
                                 : tier == 1 ? &g_pipeline.inflight_light
                                             : &g_pipeline.inflight, 1, __ATOMIC_ACQ_REL);
#endif
#else
#ifndef OFIS_NO_DROP_FIX
                __atomic_fetch_add(&g_pipeline.inflight, 1, __ATOMIC_ACQ_REL);
#endif
#endif
                start = snapshot;
                cnt = safe;
                break;
            }
        }

        compact_pipeline_queue_t *myq = &g_pipeline.queue;
        compact_pipeline_pool_t  *mypool = &g_pipeline.pool;
        uint32_t *myinflight = &g_pipeline.inflight;
#ifdef OFIS_HYBRID
        if (tier == 2) { myq = &g_pipeline.queue_rank;
                         mypool = &g_pipeline.pool_rank;
                         myinflight = &g_pipeline.inflight_rank; }
        else if (tier == 1) { myq = &g_pipeline.queue_light;
                              mypool = &g_pipeline.pool_light;
                              myinflight = &g_pipeline.inflight_light; }
#endif

        compact_pipeline_payload_t *pl = compact_pipeline_pool_acquire(mypool);
        if (!pl) {
            // Only on teardown; release the claim so no consumer wedges waiting.
#ifndef OFIS_NO_DROP_FIX
            if (__atomic_sub_fetch(myinflight, 1, __ATOMIC_ACQ_REL) == 0) {
                pthread_mutex_lock(&myq->lock);
                pthread_cond_broadcast(&myq->not_empty);
                pthread_mutex_unlock(&myq->lock);
            }
#endif
            return NULL;
        }

        for (uint32_t i = 0; i < cnt; i++) {
            st->roots_global_scratch[i] = (node_t)(start + i);
        }
        uint64_t local_n = 0, local_m = 0;
        struct timeval pb_t0, pb_t1;
        gettimeofday(&pb_t0, NULL);
        batch_compact_build(&st->ctx, st->roots_global_scratch, cnt, &local_n, &local_m);
        gettimeofday(&pb_t1, NULL);
        if (producer_idx >= 0 && producer_idx < (int)MAX_N_THREAD) {
            g_pipeline_producer_build_ms_by_producer[producer_idx] +=
                get_time_in_milliseconds(&pb_t0, &pb_t1);
            g_pipeline_producer_build_count_by_producer[producer_idx]++;
        }

        pl->start_node = start;
        pl->root_num = cnt;
        pl->row_bytes = ALIGN8((local_n + 1) * sizeof(edge_ptr));
        pl->col_bytes = ALIGN8(local_m * sizeof(node_t));
        compact_pipeline_payload_ensure_root_capacity(pl, cnt);
        memcpy(pl->roots, st->ctx.local_roots, (size_t)cnt * sizeof(node_t));
        memcpy(pl->row_ptr, st->ctx.local_row_ptr, pl->row_bytes);
        if (pl->col_bytes) {
            memcpy(pl->col_idx, st->ctx.local_col_idx, pl->col_bytes);
        }
        pl->valid = 1;

        // Publish THEN drop the claim: push is ordered before inflight-- so a consumer seeing inflight==0 finds this payload.
        const int published = compact_pipeline_queue_push(myq, pl);
        if (!published) {
            compact_pipeline_pool_release(mypool, pl);
        }
#ifdef OFIS_NO_DROP_FIX
        const uint32_t left = 1;      /* no claim was taken */
#else
        const uint32_t left =
            __atomic_sub_fetch(myinflight, 1, __ATOMIC_ACQ_REL);
#endif

        // Only the last builder (inflight->0) wakes the drain; a normal publish already signals not_empty.
        if (left == 0) {
            pthread_mutex_lock(&myq->lock);
            pthread_cond_broadcast(&myq->not_empty);
            pthread_mutex_unlock(&myq->lock);
        }
        if (!published) return NULL;
    }
    return NULL;
}

static void compact_pipeline_init(uint32_t queue_depth, uint32_t n_producers) {
    if (g_pipeline.initialized) return;
    memset(&g_pipeline, 0, sizeof(g_pipeline));
    g_pipeline.enabled = 1;
    g_pipeline.n_producers = n_producers;
    compact_pipeline_queue_init(&g_pipeline.queue, queue_depth);
    compact_pipeline_pool_init(&g_pipeline.pool, queue_depth);
#ifdef OFIS_HYBRID
    compact_pipeline_queue_init(&g_pipeline.queue_light, queue_depth);
    compact_pipeline_pool_init(&g_pipeline.pool_light, queue_depth);
    g_pipeline.inflight_light = 0;
    compact_pipeline_queue_init(&g_pipeline.queue_rank, queue_depth);
    compact_pipeline_pool_init(&g_pipeline.pool_rank, queue_depth);
    g_pipeline.inflight_rank = 0;
#endif
    g_pipeline.producer_states = calloc(n_producers, sizeof(compact_producer_state_t));
    g_pipeline.producer_threads = calloc(n_producers, sizeof(pthread_t));
    if (!g_pipeline.producer_states || !g_pipeline.producer_threads) {
        printf(ANSI_COLOR_RED "Error: pipeline producer alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    for (uint32_t i = 0; i < n_producers; i++) {
        compact_producer_state_init(&g_pipeline.producer_states[i], global_g->n);
        int rc = pthread_create(&g_pipeline.producer_threads[i], NULL,
                                compact_producer_thread_fn,
                                &g_pipeline.producer_states[i]);
        if (rc != 0) {
            printf(ANSI_COLOR_RED "Error: pipeline pthread_create failed (rc=%d)\n" ANSI_COLOR_RESET, rc);
            exit(1);
        }
    }
    g_pipeline.initialized = 1;
}

static void compact_pipeline_destroy(void) {
    if (!g_pipeline.initialized) return;
    compact_pipeline_queue_shutdown(&g_pipeline.queue);
    pthread_mutex_lock(&g_pipeline.pool.lock);
    pthread_cond_broadcast(&g_pipeline.pool.available);
    pthread_mutex_unlock(&g_pipeline.pool.lock);
#ifdef OFIS_HYBRID
    compact_pipeline_queue_shutdown(&g_pipeline.queue_light);
    pthread_mutex_lock(&g_pipeline.pool_light.lock);
    pthread_cond_broadcast(&g_pipeline.pool_light.available);
    pthread_mutex_unlock(&g_pipeline.pool_light.lock);
    compact_pipeline_queue_shutdown(&g_pipeline.queue_rank);
    pthread_mutex_lock(&g_pipeline.pool_rank.lock);
    pthread_cond_broadcast(&g_pipeline.pool_rank.available);
    pthread_mutex_unlock(&g_pipeline.pool_rank.lock);
#endif

    for (uint32_t i = 0; i < g_pipeline.n_producers; i++) {
        pthread_join(g_pipeline.producer_threads[i], NULL);
        compact_producer_state_free(&g_pipeline.producer_states[i]);
    }
    free(g_pipeline.producer_threads);
    free(g_pipeline.producer_states);

    compact_pipeline_queue_destroy(&g_pipeline.queue);
    compact_pipeline_pool_destroy(&g_pipeline.pool);
#ifdef OFIS_HYBRID
    compact_pipeline_queue_destroy(&g_pipeline.queue_light);
    compact_pipeline_pool_destroy(&g_pipeline.pool_light);
    compact_pipeline_queue_destroy(&g_pipeline.queue_rank);
    compact_pipeline_pool_destroy(&g_pipeline.pool_rank);
#endif
    memset(&g_pipeline, 0, sizeof(g_pipeline));
}

// Statistics returned by compact_payload_acquire.
typedef struct compact_acquire_stats_t {
    int from_pipeline;       /* 1 = queue hit, 0 = inline fallback (or no work). */
    double inline_build_ms;  /* Time spent inside batch_compact_build during fallback. */
} compact_acquire_stats_t;

// Bulk prebuild (OFIS_PREBUILD_ALL): pass 1 records batch boundaries serially, pass 2 builds them in parallel.
typedef struct prebuilt_batch_t {
    node_t   *roots;
    edge_ptr *row_ptr;
    node_t   *col_idx;
    uint32_t  start_node;   /* roots are [start_node, start_node + root_num) */
    uint32_t  root_num;
    uint64_t  row_bytes;
    uint64_t  col_bytes;
} prebuilt_batch_t;

static prebuilt_batch_t *g_prebuilt       = NULL;
static uint64_t          g_prebuilt_count = 0;
static uint64_t          g_prebuild_cursor = 0;  /* pass-2 work cursor        */
static uint32_t          g_prebuilt_next  = 0;   /* dispatch cursor           */
static int               g_prebuilt_ready = 0;
static double            g_prebuild_ms    = 0.0;

#ifdef OFIS_HYBRID
// First root id whose degree < thr (degree is DESC along id order).
static node_t compute_degree_cut(edge_ptr thr) {
    uint64_t lo = 0, hi = g_total_roots;
    while (lo < hi) {
        uint64_t mid = (lo + hi) >> 1;
        edge_ptr d = global_g->row_ptr[mid + 1] - global_g->row_ptr[mid];
        if (d >= thr) lo = mid + 1; else hi = mid;
    }
    return (node_t)lo;
}
static void compute_hybrid_split(void) {
#if defined(BATCH_TIER_BY_COUNT) && defined(BATCH_DEG_BUDGET)
    // g_split/g_split2 = root positions where the budget batch first reaches GRAN_IG_BATCH_N / GRAN_RANK_BATCH_N roots.
    g_split  = (node_t)g_total_roots;
    g_split2 = (node_t)g_total_roots;
    for (uint64_t pos = 0; pos < (uint64_t)g_total_roots; ) {
        const edge_ptr deg = global_g->row_ptr[pos + 1] - global_g->row_ptr[pos];
        uint32_t k;
        if (deg >= BATCH_HEAVY_THRESHOLD) k = 1;
        else k = batch_count_by_deg_budget((node_t)pos, (uint64_t)g_total_roots - pos);
        if (g_split == (node_t)g_total_roots && k >= (uint32_t)GRAN_IG_BATCH_N) g_split = (node_t)pos;
        if (k >= (uint32_t)GRAN_RANK_BATCH_N) { g_split2 = (node_t)pos; break; }
        pos += k;
    }
    if (g_split2 < g_split) g_split2 = g_split;
#else
    g_split  = compute_degree_cut(GRAN_SPLIT_THRESHOLD);   /* DPU | (IG,RANK) */
    g_split2 = compute_degree_cut(GRAN_RANK_THRESHOLD);    /* (DPU,IG) | RANK */
    if (g_split2 < g_split) g_split2 = g_split;
#endif
}
#endif
static uint64_t          g_prebuild_bytes = 0;

static void *prebuild_worker_fn(void *argp) {
    (void)argp;
    batch_compact_ctx_t ctx;
    batch_compact_ctx_init(&ctx, global_g->n);
    node_t *scratch = NULL;
    uint32_t scratch_cap = 0;

    for (;;) {
        const uint64_t i = __atomic_fetch_add(&g_prebuild_cursor, 1, __ATOMIC_RELAXED);
        if (i >= g_prebuilt_count) break;
        prebuilt_batch_t *b = &g_prebuilt[i];

        if (b->root_num > scratch_cap) {
            scratch_cap = b->root_num;
            scratch = realloc(scratch, (size_t)scratch_cap * sizeof(node_t));
            if (!scratch) {
                printf(ANSI_COLOR_RED "Error: prebuild scratch alloc failed\n" ANSI_COLOR_RESET);
                exit(1);
            }
        }
        for (uint32_t k = 0; k < b->root_num; k++) scratch[k] = b->start_node + k;

        uint64_t local_n = 0, local_m = 0;
        batch_compact_build(&ctx, scratch, b->root_num, &local_n, &local_m);

        const uint64_t roots_bytes = ALIGN8((uint64_t)b->root_num * sizeof(node_t));
        b->row_bytes = ALIGN8((local_n + 1) * sizeof(edge_ptr));
        b->col_bytes = ALIGN8(local_m * sizeof(node_t));
        b->roots   = malloc((size_t)roots_bytes);
        b->row_ptr = malloc((size_t)b->row_bytes);
        b->col_idx = b->col_bytes ? malloc((size_t)b->col_bytes) : NULL;
        if (!b->roots || !b->row_ptr || (b->col_bytes && !b->col_idx)) {
            printf(ANSI_COLOR_RED
                   "Error: prebuild out of memory at batch %" PRIu64 " / %" PRIu64
                   " (holding %.2f GB)\n" ANSI_COLOR_RESET,
                   i, g_prebuilt_count,
                   (double)__atomic_load_n(&g_prebuild_bytes, __ATOMIC_RELAXED) / (1024.0*1024.0*1024.0));
            exit(1);
        }
        memcpy(b->roots,   ctx.local_roots,   (size_t)b->root_num * sizeof(node_t));
        memcpy(b->row_ptr, ctx.local_row_ptr, (size_t)b->row_bytes);
        if (b->col_bytes) memcpy(b->col_idx, ctx.local_col_idx, (size_t)b->col_bytes);
        __atomic_fetch_add(&g_prebuild_bytes,
                           roots_bytes + b->row_bytes + b->col_bytes, __ATOMIC_RELAXED);
    }

    free(scratch);
    batch_compact_ctx_free(&ctx);
    return NULL;
}

#ifdef BATCH_DEG_BUDGET
// Size a batch by degree-sum budget: pack degree-DESC roots while sum <= BATCH_DEG_BUDGET (>=1 root), capped at ROOTS_PER_DPU.
static uint32_t batch_count_by_deg_budget(node_t start, uint64_t remain) {
    edge_ptr sum = global_g->row_ptr[start + 1] - global_g->row_ptr[start];
    uint32_t cnt = 1;
    while ((uint64_t)cnt < remain && cnt < ROOTS_PER_DPU) {
        const node_t nx = start + cnt;
        const edge_ptr d = global_g->row_ptr[nx + 1] - global_g->row_ptr[nx];
        if (sum + d > (edge_ptr)(BATCH_DEG_BUDGET)) break;
        sum += d; cnt++;
    }
    return cnt;
}
#endif

// BATCH_TIER_BY_COUNT: route granularity by batch root count n into DPU/IG/RANK tiers. Requires BATCH_DEG_BUDGET.
#ifndef GRAN_IG_BATCH_N
#define GRAN_IG_BATCH_N 32
#endif
#ifndef GRAN_RANK_BATCH_N
#define GRAN_RANK_BATCH_N 128
#endif

static void ofis_prebuild_all(void) {
    if (!g_use_batch_compact) return;   /* resident graph: nothing to build */

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);

#ifdef OFIS_HYBRID
    compute_hybrid_split();   /* g_split must be known before the pass-1 clamp */
#endif

#ifdef OFIS_PREBUILD_BATCH
    const uint32_t base_batch = (OFIS_PREBUILD_BATCH < 1) ? 1 : (uint32_t)(OFIS_PREBUILD_BATCH);
#else
    const uint32_t base_batch = g_auto_batch_size;
#endif

    // --- pass 1: batch boundaries ---
    const uint32_t n_words = (global_g->n + 31U) >> 5;
    uint32_t *bitmap = calloc((size_t)n_words, sizeof(uint32_t));
    uint8_t  *marks  = calloc((size_t)n_words, sizeof(uint8_t));
    uint32_t *words  = malloc((size_t)n_words * sizeof(uint32_t));
    uint32_t  touched = 0;
    if (!bitmap || !marks || !words) {
        printf(ANSI_COLOR_RED "Error: prebuild prefix helper alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }

    uint64_t cap = 4096;
    g_prebuilt = calloc((size_t)cap, sizeof(prebuilt_batch_t));
    if (!g_prebuilt) {
        printf(ANSI_COLOR_RED "Error: prebuild batch table alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }

    uint64_t pos = 0;
    while (pos < g_total_roots) {
        const node_t cur = (node_t)pos;
        const edge_ptr degree = global_g->row_ptr[cur + 1] - global_g->row_ptr[cur];

        const uint64_t remain = g_total_roots - pos;
#if defined(BATCH_TIER_BY_COUNT)
        uint32_t want;
        if (degree >= BATCH_HEAVY_THRESHOLD) {
            want = 1;
        } else {
            want = batch_count_by_deg_budget(cur, remain);
        }
        (void)base_batch;
#elif defined(BATCH_DEG_BUDGET)
        uint32_t want = batch_count_by_deg_budget(cur, remain);
        (void)base_batch; (void)degree;
#else
        uint32_t batch_size = base_batch;
        if (degree >= BATCH_HEAVY_THRESHOLD) {
            batch_size = base_batch / HEAVY_NODE_DIVISOR;
            if (batch_size == 0) batch_size = 1;
        }
        uint32_t want = (remain > batch_size) ? batch_size : (uint32_t)remain;
#endif

#if defined(OFIS_HYBRID) && !defined(BATCH_TIER_BY_COUNT)
        // Clamp so a batch never straddles a tier cut (dispatch cursors need disjoint tiers).
        if (pos < g_split  && pos + want > g_split)  want = (uint32_t)(g_split  - pos);
        if (pos < g_split2 && pos + want > g_split2) want = (uint32_t)(g_split2 - pos);
#endif

        edge_ptr est_m = 0;
        const uint32_t safe = find_compact_safe_prefix_count(
            cur, want, bitmap, marks, words, &touched, &est_m);
        if (safe == 0) {
            printf(ANSI_COLOR_RED
                   "Error: root %u alone exceeds compact DPU_M bound (prebuild)\n"
                   ANSI_COLOR_RESET, cur);
            exit(1);
        }

        if (g_prebuilt_count == cap) {
            cap *= 2;
            prebuilt_batch_t *grown = realloc(g_prebuilt, (size_t)cap * sizeof(prebuilt_batch_t));
            if (!grown) {
                printf(ANSI_COLOR_RED "Error: prebuild batch table grow failed\n" ANSI_COLOR_RESET);
                exit(1);
            }
            g_prebuilt = grown;
            memset(&g_prebuilt[cap / 2], 0, (size_t)(cap / 2) * sizeof(prebuilt_batch_t));
        }
        g_prebuilt[g_prebuilt_count].start_node = cur;
        g_prebuilt[g_prebuilt_count].root_num   = safe;
        g_prebuilt_count++;
        pos += safe;
    }
#ifdef OFIS_HYBRID
    g_split_batch_idx  = g_prebuilt_count;
    g_split2_batch_idx = g_prebuilt_count;
#ifdef BATCH_TIER_BY_COUNT
    for (uint64_t i = 0; i < g_prebuilt_count; i++) {
        if (g_split_batch_idx == g_prebuilt_count && g_prebuilt[i].root_num >= GRAN_IG_BATCH_N)
            g_split_batch_idx = i;
        if (g_prebuilt[i].root_num >= GRAN_RANK_BATCH_N) { g_split2_batch_idx = i; break; }
    }
#else
    for (uint64_t i = 0; i < g_prebuilt_count; i++) {
        if (g_split_batch_idx == g_prebuilt_count && g_prebuilt[i].start_node >= g_split)
            g_split_batch_idx = i;
        if (g_prebuilt[i].start_node >= g_split2) { g_split2_batch_idx = i; break; }
    }
#endif
    if (g_split2_batch_idx < g_split_batch_idx) g_split2_batch_idx = g_split_batch_idx;
    g_prebuilt_next_heavy = 0;
    g_prebuilt_next_light = g_split_batch_idx;
    g_prebuilt_next_rank  = g_split2_batch_idx;
#endif
    free(bitmap);
    free(marks);
    free(words);

    const uint32_t n_threads = (OFIS_PREBUILD_THREADS < 1) ? 1 : (uint32_t)(OFIS_PREBUILD_THREADS);
    pthread_t *th = calloc(n_threads, sizeof(pthread_t));
    if (!th) {
        printf(ANSI_COLOR_RED "Error: prebuild thread table alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }
    for (uint32_t i = 0; i < n_threads; i++) {
        if (pthread_create(&th[i], NULL, prebuild_worker_fn, NULL) != 0) {
            printf(ANSI_COLOR_RED "Error: prebuild pthread_create failed at %u\n" ANSI_COLOR_RESET, i);
            exit(1);
        }
    }
    for (uint32_t i = 0; i < n_threads; i++) pthread_join(th[i], NULL);
    free(th);

    g_prebuilt_ready = 1;
    gettimeofday(&t1, NULL);
    g_prebuild_ms = get_time_in_milliseconds(&t0, &t1);
    printf("[OFIS] bulk prebuild: %" PRIu64 " batches (base batch=%u, %u threads), "
           "%.2f GB resident, %.3f ms\n",
           g_prebuilt_count, base_batch, n_threads,
           (double)g_prebuild_bytes / (1024.0*1024.0*1024.0), g_prebuild_ms);
}

static void ofis_prebuild_free(void) {
    if (!g_prebuilt) return;
    for (uint64_t i = 0; i < g_prebuilt_count; i++) {
        free(g_prebuilt[i].roots);
        free(g_prebuilt[i].row_ptr);
        free(g_prebuilt[i].col_idx);
    }
    free(g_prebuilt);
    g_prebuilt = NULL;
    g_prebuilt_count = 0;
    g_prebuilt_ready = 0;
}

// Hand a finished DPU the next prebuilt batch: claim an index and copy; p_count still advances for the tail check.
static int prebuilt_batch_acquire(
    compact_payload_t *out,
    node_t *start_node_out,
    uint32_t *cnt_out,
    compact_acquire_stats_t *stats_out)
{
    if (stats_out) {
        stats_out->from_pipeline = 1;
        stats_out->inline_build_ms = 0.0;
    }
#ifdef OFIS_HYBRID
    // Each tier pulls from its own cursor bounded to its region; 0 = tier empty.
    uint64_t i;
    if (g_hybrid_pull_tier == 2) {
        i = __atomic_fetch_add(&g_prebuilt_next_rank, 1, __ATOMIC_ACQ_REL);
        if (i >= g_prebuilt_count) return 0;
    } else if (g_hybrid_pull_tier == 1) {
        i = __atomic_fetch_add(&g_prebuilt_next_light, 1, __ATOMIC_ACQ_REL);
        if (i >= g_split2_batch_idx) return 0;
    } else {
        i = __atomic_fetch_add(&g_prebuilt_next_heavy, 1, __ATOMIC_ACQ_REL);
        if (i >= g_split_batch_idx) return 0;
    }
#else
    const uint64_t i = __atomic_fetch_add(&g_prebuilt_next, 1, __ATOMIC_ACQ_REL);
    if (i >= g_prebuilt_count) return 0;
#endif

    const prebuilt_batch_t *b = &g_prebuilt[i];
    out->valid     = 1;
    out->root_num  = b->root_num;
    out->row_bytes = b->row_bytes;
    out->col_bytes = b->col_bytes;
    compact_payload_ensure_root_capacity(out, b->root_num);
    memcpy(out->roots,   b->roots,   (size_t)b->root_num * sizeof(node_t));
    memcpy(out->row_ptr, b->row_ptr, (size_t)b->row_bytes);
    if (b->col_bytes) memcpy(out->col_idx, b->col_idx, (size_t)b->col_bytes);

    *start_node_out = b->start_node;
    *cnt_out        = b->root_num;
    __atomic_fetch_add(&p_count, b->root_num, __ATOMIC_ACQ_REL);
    return 1;
}

// True when the worker gets ready-made payloads (pipeline or bulk prebuild).
static inline int ofis_payload_path_enabled(void) {
#ifdef OFIS_PREBUILD_ALL
    if (g_prebuilt_ready) return 1;
#endif
    return g_pipeline.enabled;
}

// Obtain a compact payload for one DPU (never blocks; empty queue falls back to inline build). Returns 1 with start_node/cnt, 0 on no work.
static int compact_payload_acquire(
    compact_payload_t *out,
    batch_compact_ctx_t *worker_ctx,
    uint32_t *batch_bitmap,
    uint8_t  *batch_touched_marks,
    uint32_t *batch_touched_words,
    uint32_t *batch_touched_count,
    node_t   *start_node_out,
    uint32_t *cnt_out,
    compact_acquire_stats_t *stats_out)
{
    if (stats_out) {
        stats_out->from_pipeline = 0;
        stats_out->inline_build_ms = 0.0;
    }

#ifdef OFIS_PREBUILD_ALL
    if (g_prebuilt_ready) {
        return prebuilt_batch_acquire(out, start_node_out, cnt_out, stats_out);
    }
#endif

#ifdef OFIS_HYBRID
    compact_pipeline_queue_t *cq  = g_hybrid_pull_tier == 2 ? &g_pipeline.queue_rank
                                  : g_hybrid_pull_tier == 1 ? &g_pipeline.queue_light : &g_pipeline.queue;
    compact_pipeline_pool_t  *cp  = g_hybrid_pull_tier == 2 ? &g_pipeline.pool_rank
                                  : g_hybrid_pull_tier == 1 ? &g_pipeline.pool_light  : &g_pipeline.pool;
    uint32_t                 *cif = g_hybrid_pull_tier == 2 ? &g_pipeline.inflight_rank
                                  : g_hybrid_pull_tier == 1 ? &g_pipeline.inflight_light : &g_pipeline.inflight;
    const uint32_t my_bound = g_hybrid_pull_tier == 2 ? (uint32_t)g_total_roots
                            : g_hybrid_pull_tier == 1 ? (uint32_t)g_split2 : (uint32_t)g_split;
#else
    compact_pipeline_queue_t *cq  = &g_pipeline.queue;
    compact_pipeline_pool_t  *cp  = &g_pipeline.pool;
    uint32_t                 *cif = &g_pipeline.inflight;
    const uint32_t my_bound = (uint32_t)g_total_roots;
#endif

    if (g_pipeline.enabled) {
        compact_pipeline_payload_t *pl = compact_pipeline_queue_try_pop(cq);
        if (pl) {
            const uint64_t rn = pl->root_num;
            out->valid = 1;
            out->root_num = rn;
            out->row_bytes = pl->row_bytes;
            out->col_bytes = pl->col_bytes;
            compact_payload_ensure_root_capacity(out, rn);
            memcpy(out->roots, pl->roots, (size_t)rn * sizeof(node_t));
            memcpy(out->row_ptr, pl->row_ptr, pl->row_bytes);
            if (pl->col_bytes) memcpy(out->col_idx, pl->col_idx, pl->col_bytes);
            *start_node_out = (node_t)pl->start_node;
            *cnt_out = (uint32_t)rn;
            compact_pipeline_pool_release(cp, pl);
            if (stats_out) stats_out->from_pipeline = 1;
            return 1;
        }
    }

    for (;;) {
        uint32_t snapshot = __atomic_load_n(&p_count, __ATOMIC_ACQUIRE);
        if (snapshot >= my_bound) {
            // A producer may still owe a batch, so wait on inflight (empty queue + inflight 0 = done) instead of retiring.
            if (!g_pipeline.enabled) return 0;
#ifdef OFIS_NO_DROP_FIX
            return 0;
#else
            compact_pipeline_payload_t *late =
                compact_pipeline_queue_pop_drain_on(cq, cif);
            if (!late) return 0;   /* queue empty and nobody building: done */
            const uint64_t rn = late->root_num;
            out->valid = 1;
            out->root_num = rn;
            out->row_bytes = late->row_bytes;
            out->col_bytes = late->col_bytes;
            compact_payload_ensure_root_capacity(out, rn);
            memcpy(out->roots, late->roots, (size_t)rn * sizeof(node_t));
            memcpy(out->row_ptr, late->row_ptr, late->row_bytes);
            if (late->col_bytes) memcpy(out->col_idx, late->col_idx, late->col_bytes);
            *start_node_out = (node_t)late->start_node;
            *cnt_out = (uint32_t)rn;
            compact_pipeline_pool_release(cp, late);
            if (stats_out) stats_out->from_pipeline = 1;
            return 1;
#endif
        }
        uint64_t remain = g_total_roots - snapshot;
        node_t cur_node = snapshot;
        edge_ptr degree = global_g->row_ptr[cur_node + 1] - global_g->row_ptr[cur_node];
#if defined(OFIS_HYBRID) && defined(BATCH_TIER_BY_COUNT)
        uint32_t want;
        if (degree >= BATCH_HEAVY_THRESHOLD) want = 1;
        else want = batch_count_by_deg_budget(cur_node, remain);
#else
        uint32_t current_batch_size = g_auto_batch_size;
        if (degree >= BATCH_HEAVY_THRESHOLD) {
            current_batch_size = g_auto_batch_size / HEAVY_NODE_DIVISOR;
            if (current_batch_size == 0) current_batch_size = 1;
        }
        uint32_t want = (remain > current_batch_size) ? current_batch_size : (uint32_t)remain;
#endif
        edge_ptr est_m = 0;
        uint32_t safe = find_compact_safe_prefix_count(
            cur_node, want,
            batch_bitmap, batch_touched_marks, batch_touched_words, batch_touched_count,
            &est_m);
        if (safe == 0) {
            printf(ANSI_COLOR_RED
                   "Error: root %u alone exceeds compact DPU_M bound (inline)\n"
                   ANSI_COLOR_RESET, cur_node);
            exit(1);
        }
        uint32_t expected = snapshot;
        uint32_t desired = snapshot + safe;
        if (!__atomic_compare_exchange_n(&p_count, &expected, desired,
                                         false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            continue;
        }
        node_t *scratch = malloc((size_t)safe * sizeof(node_t));
        if (!scratch) {
            printf(ANSI_COLOR_RED "Error: inline scratch alloc failed\n" ANSI_COLOR_RESET);
            exit(1);
        }
        for (uint32_t i = 0; i < safe; i++) scratch[i] = (node_t)(snapshot + i);
        uint64_t local_n = 0, local_m = 0;
        struct timeval bt0, bt1;
        gettimeofday(&bt0, NULL);
        batch_compact_build(worker_ctx, scratch, safe, &local_n, &local_m);
        gettimeofday(&bt1, NULL);
        free(scratch);

        out->valid = 1;
        out->root_num = safe;
        out->row_bytes = ALIGN8((local_n + 1) * sizeof(edge_ptr));
        out->col_bytes = ALIGN8(local_m * sizeof(node_t));
        compact_payload_ensure_root_capacity(out, safe);
        memcpy(out->roots, worker_ctx->local_roots, (size_t)safe * sizeof(node_t));
        memcpy(out->row_ptr, worker_ctx->local_row_ptr, out->row_bytes);
        if (out->col_bytes) memcpy(out->col_idx, worker_ctx->local_col_idx, out->col_bytes);
        *start_node_out = snapshot;
        *cnt_out = safe;
        if (stats_out) {
            stats_out->from_pipeline = 0;
            stats_out->inline_build_ms = get_time_in_milliseconds(&bt0, &bt1);
        }
        return 1;
    }
}

uint64_t **global_offset; // [thread_id][each_dpu]

typedef struct{
    uint32_t thread_id;

    node_t n;  // number of vertices
    edge_ptr m;  // number of edges
    edge_ptr row_ptr[N];
    node_t col_idx[M];

    uint64_t root_num[NR_DPUS];  // number of search roots allocated to dpu
    struct dpu_set_t rank;
    node_t *roots[NR_DPUS];
} thread_args;

int init_off_alloc(void) {
    if (!global_offset) {
        global_offset = malloc(sizeof(*global_offset) * MAX_N_RANK);
        if (!global_offset) return -1;
        for (int t = 0; t < MAX_N_RANK; ++t) {
            global_offset[t] = malloc(sizeof(uint64_t) * NR_DPUS);
            if (!global_offset[t]) return -1;
        }
    }

    for (int t = 0; t < MAX_N_RANK; ++t) {
        memset(global_offset[t], 0, sizeof(uint64_t) * NR_DPUS);
    }
    return 0;
}

static int deg_cmp(const void *a, const void *b) {
    node_t x = *(node_t *)a;
    node_t y = *(node_t *)b;
    return global_g->row_ptr[y + 1] - global_g->row_ptr[y] - (global_g->row_ptr[x + 1] - global_g->row_ptr[x]);
}
static int node_t_cmp(const void *a, const void *b) {
    node_t x = *(node_t *)a;
    node_t y = *(node_t *)b;
    return x - y;
}
static int workload_cmp(const void *a, const void *b) {
    node_t x = *(node_t *)a;
    node_t y = *(node_t *)b;
    return workload[y] - workload[x];
}

#ifdef MORE_ACCURATE_MODEL
static inline double predict_workload(Graph *g, node_t root) {
    double deg = g->row_ptr[root + 1] - g->row_ptr[root];
    edge_ptr l = g->row_ptr[root], r = g->row_ptr[root + 1];
    while (l < r) {
        edge_ptr mid = (l + r) >> 1;
        if (g->col_idx[mid] < root) {
            l = mid + 1;
        }
        else {
            r = mid;
        }
    }
    double eff_deg = eff_deg = l - g->row_ptr[root];
    if (deg > MRAM_BUF_SIZE) {
        printf(ANSI_COLOR_RED "Error: deg too large\n" ANSI_COLOR_RESET);
        exit(1);
    }
    if (eff_deg > BITMAP_SIZE * 32) {
        printf(ANSI_COLOR_RED "Error: eff_deg too large\n" ANSI_COLOR_RESET);
        exit(1);
    }
    double avg_deg = 0;
    for (edge_ptr i = g->row_ptr[root]; i < g->row_ptr[root + 1]; i++) {
        node_t neighbor = g->col_idx[i];
        avg_deg += g->row_ptr[neighbor + 1] - g->row_ptr[neighbor];
    }
    avg_deg /= g->row_ptr[root + 1] - g->row_ptr[root];
    double n = global_g->n;
    (void)deg;
    (void)eff_deg;
    (void)avg_deg;
    (void)n;
#if defined(CLIQUE2)
    return eff_deg;
#elif defined(CLIQUE3)
    return eff_deg * eff_deg * avg_deg + 100;
#elif defined(CLIQUE4)
    return eff_deg * eff_deg * eff_deg * avg_deg * avg_deg * avg_deg + 100;
#elif defined(CLIQUE5)
    return eff_deg * eff_deg * eff_deg * eff_deg * avg_deg * avg_deg * avg_deg * avg_deg * avg_deg * avg_deg + 100;
#elif defined(CYCLE4)
    return eff_deg * eff_deg * avg_deg + 100;
#elif defined(HOUSE5)
    return eff_deg * deg * avg_deg * (2 + deg / n + avg_deg / n) + 100;
#elif defined(HOUSE5_X)
    return eff_deg * deg * avg_deg * avg_deg / n + 100;
#elif defined(HOUSE5_Y)
    return eff_deg * (deg + avg_deg + (deg * avg_deg / n) * (deg * avg_deg / n + avg_deg)) + 100;
#elif defined(TRI_TRI6)
    return eff_deg * eff_deg * avg_deg * (deg + 3 * avg_deg + (deg + avg_deg) * avg_deg / n) + 100;
#elif defined(QG3)
    return eff_deg * eff_deg * avg_deg + 100;
#elif defined(PENTAGON)
    return eff_deg * eff_deg * avg_deg * avg_deg + 100;
#elif defined(HOURGLASS)
    return eff_deg * eff_deg * eff_deg * avg_deg * avg_deg + 100;
#elif defined(K4_TAIL5)
    return eff_deg * (deg + avg_deg + (deg * avg_deg / n) * (deg * avg_deg / n + avg_deg)) + 100; /* same kernel structure as HOUSE5_Y */
#elif defined(P_P6)
    return eff_deg * (deg + avg_deg + avg_deg * (deg + 2 * avg_deg + (deg * avg_deg) / n)) + 100;
#endif
}
#else
static inline double predict_workload(Graph *g, node_t root) {
    double deg = g->row_ptr[root + 1] - g->row_ptr[root];
    edge_ptr l = g->row_ptr[root], r = g->row_ptr[root + 1];
    while (l < r) {
        edge_ptr mid = (l + r) >> 1;
        if (g->col_idx[mid] < root) {
            l = mid + 1;
        }
        else {
            r = mid;
        }
    }
    double eff_deg = eff_deg = l - g->row_ptr[root];
    if (deg > MRAM_BUF_SIZE) {
        printf(ANSI_COLOR_RED "Error: deg too large\n" ANSI_COLOR_RESET);
        exit(1);
    }
    if (eff_deg > BITMAP_SIZE * 32) {
        printf(ANSI_COLOR_RED "Error: eff_deg too large\n" ANSI_COLOR_RESET);
        exit(1);
    }
    double avg_deg = (double)global_g->m / global_g->n;
    double n = global_g->n;
    (void)deg;
    (void)eff_deg;
    (void)avg_deg;
    (void)n;
#if defined(CLIQUE2)
    return eff_deg;
#elif defined(CLIQUE3)
    return eff_deg * eff_deg + 100;
#elif defined(CLIQUE4)
    return eff_deg * eff_deg * eff_deg + 100;
#elif defined(CLIQUE5)
    return eff_deg * eff_deg * eff_deg * eff_deg + 100;
#elif defined(CYCLE4)
    return eff_deg * eff_deg + 100;
#elif defined(HOUSE5)
    return eff_deg * deg * (2 + deg / n + avg_deg / n) + 100;
#elif defined(HOUSE5_X)
    return eff_deg * deg * avg_deg * avg_deg / n + 100;
#elif defined(HOUSE5_Y)
    return eff_deg * (deg + avg_deg + (deg * avg_deg / n) * (deg * avg_deg / n + avg_deg)) + 100;
#elif defined(TRI_TRI6)
    return eff_deg * eff_deg * (deg + 3 * avg_deg + (deg + avg_deg) * avg_deg / n) + 100;
#elif defined(QG3)
    return eff_deg * eff_deg + 100;
#elif defined(PENTAGON)
    return eff_deg * eff_deg * avg_deg + 100;
#elif defined(HOURGLASS)
    return eff_deg * eff_deg * eff_deg * avg_deg + 100;
#elif defined(K4_TAIL5)
    return eff_deg * (deg + avg_deg + (deg * avg_deg / n) * (deg * avg_deg / n + avg_deg)) + 100; /* same kernel structure as HOUSE5_Y */
#elif defined(P_P6)
    return eff_deg * (deg + avg_deg + avg_deg * (deg + 2 * avg_deg + (deg * avg_deg) / n)) + 100;
#endif
}
#endif

static void read_input() {
    FILE *fin = fopen(DATA_PATH, "rb");
    if (fin == NULL) {
        perror("Error opening file");
        exit(1);
    }
    node_t n;
    edge_ptr m;
    edge_ptr *row_ptr = global_g->row_ptr;
    node_t *col_idx = global_g->col_idx;
    fread(&n, sizeof(node_t), 1, fin);
    fread(&m, sizeof(edge_ptr), 1, fin);
    fread(row_ptr, sizeof(edge_ptr), n, fin);
    fread(col_idx, sizeof(node_t), m, fin);
    row_ptr[n] = m;
    fclose(fin);
    global_g->n = n;
    global_g->m = m;
}

static void data_renumber() {
    static node_t rank[N];
    static node_t renumbered[N];
    Graph *tmp_g = malloc(sizeof(Graph));
    memcpy(tmp_g, global_g, sizeof(Graph));
    for (node_t i = 0; i < global_g->n; i++) {
        rank[i] = i;
    }
    qsort(rank, global_g->n, sizeof(node_t), deg_cmp);
    for (node_t i = 0; i < global_g->n; i++) {
        renumbered[rank[i]] = i;
    }
    edge_ptr cur = 0;
    for (node_t i = 0; i < global_g->n; i++) {
        global_g->row_ptr[i] = cur;
        node_t node = rank[i];
        for (edge_ptr j = tmp_g->row_ptr[node]; j < tmp_g->row_ptr[node + 1]; j++) {
            global_g->col_idx[cur++] = renumbered[tmp_g->col_idx[j]];
        }
        qsort(global_g->col_idx + global_g->row_ptr[i], cur - global_g->row_ptr[i], sizeof(node_t), node_t_cmp);
    }
    free(tmp_g);
}

#ifdef DEGCYCLE_CPU
// Per-root CPU timing: run KERNEL_FUNC per root, record wall-ns. Env: DC_DEGCAP, DC_THREADS (default 32).
#include <time.h>
extern ans_t KERNEL_FUNC(Graph *g, node_t root);
static inline uint64_t dc_now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec; }
typedef struct { uint64_t *ns; node_t cap; uint32_t stride, off, reps; ans_t acc; } dc_arg_t;
static void *dc_worker(void *p){
    dc_arg_t *a=(dc_arg_t*)p; ans_t acc=0;
    const uint32_t reps = a->reps ? a->reps : 1;
    for(node_t r=a->off; r<global_g->n; r+=a->stride){
        edge_ptr d = global_g->row_ptr[r+1]-global_g->row_ptr[r];
        if(a->cap && d>a->cap){ a->ns[r]=0; continue; }
        // min-of-reps: the minimum wall time (least OS jitter) best reflects the true per-root work.
        uint64_t best=~0ULL;
        for(uint32_t rp=0; rp<reps; rp++){
            uint64_t t0=dc_now_ns();
            ans_t v = KERNEL_FUNC(global_g, r);
            uint64_t dt=dc_now_ns()-t0;
            if(dt<best) best=dt;
            if(rp==0) acc += v;
        }
        a->ns[r]=best;
    }
    a->acc=acc; return NULL;
}
void degcycle_cpu_run(Graph *g){
    global_g=g;
    read_input(); data_renumber();
    node_t n=global_g->n;
    uint64_t *ns=calloc(n,sizeof(uint64_t));
    node_t   cap = getenv("DC_DEGCAP")? (node_t)strtoul(getenv("DC_DEGCAP"),NULL,10):0;
    uint32_t nthr= getenv("DC_THREADS")? (uint32_t)strtoul(getenv("DC_THREADS"),NULL,10):32;
    if(nthr<1) nthr=1; if(nthr>256) nthr=256;
    uint32_t reps= getenv("DC_REPS")? (uint32_t)strtoul(getenv("DC_REPS"),NULL,10):1;
    if(reps<1) reps=1; if(reps>1000) reps=1000;
    printf("[DEGCYCLE_CPU] n=%u pattern=%s data=%s cap=%u threads=%u reps=%u (min-of-reps)\n",
           n,PATTERN_NAME,DATA_NAME,(unsigned)cap,nthr,reps);
    pthread_t th[256]; dc_arg_t arg[256];
    uint64_t t0=dc_now_ns();
    for(uint32_t t=0;t<nthr;t++){ arg[t].ns=ns; arg[t].cap=cap; arg[t].stride=nthr; arg[t].off=t; arg[t].reps=reps; arg[t].acc=0;
        pthread_create(&th[t],NULL,dc_worker,&arg[t]); }
    ans_t chk=0;
    for(uint32_t t=0;t<nthr;t++){ pthread_join(th[t],NULL); chk+=arg[t].acc; }
    printf("[DEGCYCLE_CPU] measured %u roots in %.1f s, total_ans=%lu\n",
           n,(dc_now_ns()-t0)/1e9,(unsigned long)chk);
    char path[256]; snprintf(path,sizeof(path),"./result/degcpu_%s_%s.csv",PATTERN_NAME,DATA_NAME);
    FILE *f=fopen(path,"w"); fprintf(f,"node,degree,ns\n");
    for(node_t v=0;v<n;v++){ edge_ptr d=global_g->row_ptr[v+1]-global_g->row_ptr[v];
        if(cap && d>cap) continue; fprintf(f,"%u,%u,%lu\n",v,(unsigned)d,(unsigned long)ns[v]); }
    fclose(f); free(ns);
    printf("[DEGCYCLE_CPU] wrote %s\n",path);
}
#endif

// check whether node n is already in this DPU's bitmap
static inline bool check_in_bitmap(node_t n, uint32_t bitmap[N >> 5]) {
    return bitmap[n >> 5] & (1 << (n & 31));
}

// check for remaining space; register the data if it fits
typedef uint32_t(*bitmap_t)[N >> 5];
static bool update_alloc_info(uint32_t dpu_id, node_t n, edge_ptr *m_count, bitmap_t bitmap) {
    // check condition
    if (global_g->root_num[dpu_id] == DPU_ROOT_NUM) {
        return false; // this DPU's task slots are full
    }
    edge_ptr dpu_m_count = m_count[dpu_id];
    if (!check_in_bitmap(n, bitmap[dpu_id])) {
        // if node n is not in the bitmap, add memory for n's neighbor list
        dpu_m_count += global_g->row_ptr[n + 1] - global_g->row_ptr[n];
    }

    for (edge_ptr i = global_g->row_ptr[n]; i < global_g->row_ptr[n + 1]; i++) {
        node_t neighbor = global_g->col_idx[i]; // one of n's neighbors
        if (!check_in_bitmap(neighbor, bitmap[dpu_id])) {
            // if the neighbor is not in the bitmap, add memory for its neighbor list
            dpu_m_count += global_g->row_ptr[neighbor + 1] - global_g->row_ptr[neighbor];
        }
    }
    if (dpu_m_count > DPU_M) {
        return false;
    }

    // allocate

    // add n to the root list
    global_g->roots[dpu_id][global_g->root_num[dpu_id]++] = n;
    // update memory usage
    m_count[dpu_id] = dpu_m_count;
    // mark bitmap; register root n
    bitmap[dpu_id][n >> 5] |= (1 << (n & 31));
    // register all of n's neighbors in the bitmap too
    for (edge_ptr i = global_g->row_ptr[n]; i < global_g->row_ptr[n + 1]; i++) {
        node_t neighbor = global_g->col_idx[i];
        bitmap[dpu_id][neighbor >> 5] |= (1 << (neighbor & 31));
    }
    return true;
}

static inline void prefix_mark_node(uint32_t *bitmap, uint8_t *touched_marks, uint32_t *touched_words, uint32_t *touched_count, node_t n) {
    const uint32_t word_idx = n >> 5;
    if (!touched_marks[word_idx]) {
        touched_marks[word_idx] = 1;
        touched_words[(*touched_count)++] = word_idx;
    }
    bc_set_bit(bitmap, n);
}

static uint32_t find_compact_safe_prefix_count(
    node_t start_node,
    uint32_t max_batch_size,
    uint32_t *bitmap,
    uint8_t *touched_marks,
    uint32_t *touched_words,
    uint32_t *touched_count,
    edge_ptr *local_m_out
) {
    edge_ptr local_m = 0;
    uint32_t safe_count = 0;

    for (uint32_t i = 0; i < *touched_count; i++) {
        const uint32_t word_idx = touched_words[i];
        bitmap[word_idx] = 0;
        touched_marks[word_idx] = 0;
    }
    *touched_count = 0;

    for (uint32_t i = 0; i < max_batch_size; i++) {
        const node_t root = start_node + i;
        edge_ptr next_local_m = local_m;

        if (!bc_check_bit(bitmap, root)) {
            next_local_m += global_g->row_ptr[root + 1] - global_g->row_ptr[root];
        }

        for (edge_ptr j = global_g->row_ptr[root]; j < global_g->row_ptr[root + 1]; j++) {
            const node_t neighbor = global_g->col_idx[j];
            if (!bc_check_bit(bitmap, neighbor)) {
                next_local_m += global_g->row_ptr[neighbor + 1] - global_g->row_ptr[neighbor];
            }
            if (next_local_m > DPU_M) {
                break;
            }
        }

        if (next_local_m > DPU_M) {
            break;
        }

        local_m = next_local_m;
        prefix_mark_node(bitmap, touched_marks, touched_words, touched_count, root);
        for (edge_ptr j = global_g->row_ptr[root]; j < global_g->row_ptr[root + 1]; j++) {
            prefix_mark_node(bitmap, touched_marks, touched_words, touched_count, global_g->col_idx[j]);
        }
        safe_count++;
    }

    *local_m_out = local_m;
    return safe_count;
}

void queue_init();
void push_to_queue(uint32_t dpu_id, double work_load);
uint32_t pop_from_queue();

static node_t g_alloc_cursor = 0;      /* next root (in sorted order) to assign */
static int    g_alloc_sorted_done = 0; /* workload sort + roots[] malloc done once */
static node_t g_alloc_order[N];        /* root ids sorted by predicted workload desc */

static node_t data_allocate(bitmap_t bitmap) {
    static edge_ptr m_count[NR_DPUS];   // edges put in dpu
    static double dpu_workload[NR_DPUS];

    memset(bitmap, 0, (size_t)(N >> 3) * NR_DPUS);
    memset(m_count, 0, sizeof(m_count));
    memset(dpu_workload, 0, sizeof(dpu_workload));

    if (!g_alloc_sorted_done) {
        for (uint32_t i = 0; i < NR_DPUS; i++) {
            global_g->roots[i] = malloc(DPU_ROOT_NUM * sizeof(node_t));
        }
        for (node_t i = 0; i < global_g->n; i++) {
            g_alloc_order[i] = i;
            workload[i] = predict_workload(global_g, i);
        }
        qsort(g_alloc_order, global_g->n, sizeof(node_t), workload_cmp);
        g_alloc_sorted_done = 1;
    }
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        global_g->root_num[i] = 0;
    }

    queue_init();
    uint32_t full_dpu_ct = 0;
    node_t i = g_alloc_cursor;
    for (; i < global_g->n; i++) {
        int placed = 0;
        while (full_dpu_ct != NR_DPUS) {
            uint32_t cur_dpu = pop_from_queue();
            if (update_alloc_info(cur_dpu, g_alloc_order[i], m_count, bitmap)) {
                dpu_workload[cur_dpu] += workload[g_alloc_order[i]];
                push_to_queue(cur_dpu, dpu_workload[cur_dpu]);
                placed = 1;
                break;
            }
            else {
                full_dpu_ct++;
            }
        }
        if (!placed) break;   // all DPUs full: stop this round at root i
    }
    node_t assigned = i - g_alloc_cursor;
    g_alloc_cursor = i;

    if (assigned == 0 && g_alloc_cursor < global_g->n) {
        printf(ANSI_COLOR_RED "Error: a single root's work exceeds all DPUs\n" ANSI_COLOR_RESET);
        exit(1);
    }
    return assigned;
}

double get_time_in_milliseconds(struct timeval *start, struct timeval *end) {
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 + (double)(end->tv_usec - start->tv_usec) / 1000.0;
}

#ifdef CPU_COMPACTION
// Parallel host-side compaction worker: each thread builds a stride of DPUs' CSRs.
typedef struct cpu_compact_arg_t {
    uint32_t tid;
    uint32_t n_threads;
    edge_ptr (*dpu_row_ptr)[DPU_N * 2];
    node_t   (*dpu_col_idx)[DPU_M * 2];
    node_t   (*dpu_roots)[DPU_ROOT_NUM];
} cpu_compact_arg_t;

static void *cpu_compact_worker(void *argp) {
    cpu_compact_arg_t *a = (cpu_compact_arg_t *)argp;
    batch_compact_ctx_t cctx;
    batch_compact_ctx_init(&cctx, global_g->n);
    for (uint32_t i = a->tid; i < NR_DPUS; i += a->n_threads) {
        uint64_t local_n = 0, local_m = 0;
        const uint64_t rn = global_g->root_num[i];
        batch_compact_build(&cctx, global_g->roots[i], rn, &local_n, &local_m);
        if (rn)      memcpy(a->dpu_roots[i],   cctx.local_roots,   (size_t)rn * sizeof(node_t));
        memcpy(a->dpu_row_ptr[i], cctx.local_row_ptr, (size_t)(local_n + 1) * sizeof(edge_ptr));
        if (local_m) memcpy(a->dpu_col_idx[i], cctx.local_col_idx, (size_t)local_m * sizeof(node_t));
    }
    batch_compact_ctx_free(&cctx);
    return NULL;
}
#endif /* CPU_COMPACTION */

void data_compact(struct dpu_set_t set, bitmap_t bitmap) {


    FILE *fp_time_cpt;
    fp_time_cpt = fopen("./result/" "result_"PATTERN_NAME "_" DATA_NAME ".txt", "w");
    struct timeval start_time, end_time;

    edge_ptr(*dpu_row_ptr)[DPU_N * 2];
    dpu_row_ptr = malloc(NR_DPUS * DPU_N * 2 * sizeof(edge_ptr));
    node_t(*dpu_col_idx)[DPU_M * 2];
    dpu_col_idx = malloc(NR_DPUS * DPU_M * 2 * sizeof(node_t));
    node_t(*dpu_roots)[DPU_ROOT_NUM];
    dpu_roots = malloc(NR_DPUS * DPU_ROOT_NUM * sizeof(node_t));

    struct dpu_set_t dpu;
    uint32_t each_dpu;

#ifndef CPU_COMPACTION
    gettimeofday(&start_time, NULL);

    DPU_ASSERT(dpu_load(set, DPU_ALLOC_BINARY, NULL));

    gettimeofday(&end_time, NULL);
    printf("Allocation Binary Loading Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));

    uint64_t mode = 0;
    double total_compacting_time = 0.0;

#ifdef COMPACT_XFER_PROFILE
    // Split each compaction mode into exec (dpu_launch) / xfer (DMA) / host (remainder), accumulated over rounds.
    struct timeval _le0, _le1, _lx0, _lx1; double m_exec[3] = {0}, m_xfer[3] = {0};
#define CPT_LAUNCH(mi) do { gettimeofday(&_le0, NULL); DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS)); \
        gettimeofday(&_le1, NULL); m_exec[mi] += get_time_in_milliseconds(&_le0, &_le1); } while (0)
#define CPT_XFER(mi, stmt) do { gettimeofday(&_lx0, NULL); stmt; \
        gettimeofday(&_lx1, NULL); m_xfer[mi] += get_time_in_milliseconds(&_lx0, &_lx1); } while (0)
#else
#define CPT_LAUNCH(mi) DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS))
#define CPT_XFER(mi, stmt) stmt
#endif
    gettimeofday(&start_time, NULL);

    CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "mode", 0, &mode, sizeof(uint64_t), DPU_XFER_DEFAULT)));
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, bitmap[each_dpu]));
    }
    CPT_XFER(0, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "bitmap", 0, (N >> 5) * sizeof(uint32_t), DPU_XFER_DEFAULT)));
    static uint32_t zero[N >> 5];
    memset(zero, 0, sizeof(zero));
    CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "involve_bitmap", 0, zero, sizeof(zero), DPU_XFER_DEFAULT)));
    uint64_t start = 0;
    while (start < global_g->n) {
        uint64_t size = 0;
        while (start + size < global_g->n && global_g->row_ptr[start + size + 1] - global_g->row_ptr[start] < PARTITION_M && ALIGN8(size + 1) * sizeof(edge_ptr) < (1 << 22)) {
            size++;
        }
        CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "start", 0, &start, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "size", 0, &size, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "row_ptr", 0, &global_g->row_ptr[start], ALIGN8((size + 1) * sizeof(edge_ptr)), DPU_XFER_DEFAULT)));
        CPT_XFER(0, DPU_ASSERT(dpu_broadcast_to(set, "col_idx", 0, &global_g->col_idx[global_g->row_ptr[start]], ALIGN8((global_g->row_ptr[start + size] - global_g->row_ptr[start]) * sizeof(node_t)), DPU_XFER_DEFAULT)));
        CPT_LAUNCH(0);
        start += size;
    }

    gettimeofday(&end_time, NULL);
    printf("Mode[0] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_cpt, "Mode[0] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
#ifdef COMPACT_XFER_PROFILE
    printf("Mode[0] split (ms): exec=%.6f xfer=%.6f host=%.6f\n", m_exec[0], m_xfer[0], get_time_in_milliseconds(&start_time, &end_time) - m_exec[0] - m_xfer[0]);
#endif
    total_compacting_time += get_time_in_milliseconds(&start_time, &end_time);

    mode = 1;

    gettimeofday(&start_time, NULL);

    CPT_XFER(1, DPU_ASSERT(dpu_broadcast_to(set, "mode", 0, &mode, sizeof(uint64_t), DPU_XFER_DEFAULT)));
    uint64_t n_size = global_g->n;
    CPT_XFER(1, DPU_ASSERT(dpu_broadcast_to(set, "size", 0, &n_size, sizeof(uint64_t), DPU_XFER_DEFAULT)));
    DPU_FOREACH(set, dpu, each_dpu) {
        uint64_t root_num = global_g->root_num[each_dpu];
        CPT_XFER(1, DPU_ASSERT(dpu_copy_to(dpu, "root_size", 0, &root_num, sizeof(uint64_t))));
    }
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->roots[each_dpu]));
    }
    CPT_XFER(1, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "roots", 0, DPU_ROOT_NUM * sizeof(node_t), DPU_XFER_DEFAULT)));
    CPT_LAUNCH(1);
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_roots[each_dpu]));
    }
    CPT_XFER(1, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "roots", 0, DPU_ROOT_NUM * sizeof(node_t), DPU_XFER_DEFAULT)));

    gettimeofday(&end_time, NULL);
    printf("Mode[1] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_cpt, "Mode[1] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
#ifdef COMPACT_XFER_PROFILE
    printf("Mode[1] split (ms): exec=%.6f xfer=%.6f host=%.6f\n", m_exec[1], m_xfer[1], get_time_in_milliseconds(&start_time, &end_time) - m_exec[1] - m_xfer[1]);
#endif
    total_compacting_time += get_time_in_milliseconds(&start_time, &end_time);

    mode = 2;

    gettimeofday(&start_time, NULL);

    CPT_XFER(2, DPU_ASSERT(dpu_broadcast_to(set, "mode", 0, &mode, sizeof(uint64_t), DPU_XFER_DEFAULT)));
    static uint64_t processed_row_size[NR_DPUS];
    memset(processed_row_size, 0, NR_DPUS * sizeof(uint64_t));
    static uint64_t processed_col_size[NR_DPUS];
    memset(processed_col_size, 0, NR_DPUS * sizeof(uint64_t));
    static uint64_t tmp_row_size[NR_DPUS];
    static uint64_t tmp_col_size[NR_DPUS];
    start = 0;
    while (start < global_g->n) {
        uint64_t size = 0;
        while (start + size < global_g->n && global_g->row_ptr[start + size + 1] - global_g->row_ptr[start] < PARTITION_M && ALIGN8(size + 1) * sizeof(edge_ptr) < (1 << 22)) {
            size++;
        }
        CPT_XFER(2, DPU_ASSERT(dpu_broadcast_to(set, "start", 0, &start, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        CPT_XFER(2, DPU_ASSERT(dpu_broadcast_to(set, "size", 0, &size, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &processed_col_size[each_dpu]));
        }
        CPT_XFER(2, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "processed_offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        CPT_XFER(2, DPU_ASSERT(dpu_broadcast_to(set, "row_ptr", 0, &global_g->row_ptr[start], ALIGN8((size + 1) * sizeof(edge_ptr)), DPU_XFER_DEFAULT)));
        CPT_XFER(2, DPU_ASSERT(dpu_broadcast_to(set, "col_idx", 0, &global_g->col_idx[global_g->row_ptr[start]], ALIGN8((global_g->row_ptr[start + size] - global_g->row_ptr[start]) * sizeof(node_t)), DPU_XFER_DEFAULT)));
        CPT_LAUNCH(2);
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &tmp_row_size[each_dpu]));
        }
        CPT_XFER(2, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "processed_row_size", 0, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &tmp_col_size[each_dpu]));
        }
        CPT_XFER(2, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "processed_col_size", 0, sizeof(uint64_t), DPU_XFER_DEFAULT)));
        uint64_t max_row_size = 0;
        uint64_t max_col_size = 0;
        DPU_FOREACH(set, dpu, each_dpu) {
            if (tmp_row_size[each_dpu] > max_row_size) {
                max_row_size = tmp_row_size[each_dpu];
            }
            if (tmp_col_size[each_dpu] > max_col_size) {
                max_col_size = tmp_col_size[each_dpu];
            }
        }
        if (max_row_size != 0) {
            DPU_FOREACH(set, dpu, each_dpu) {
                DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_row_ptr[each_dpu][processed_row_size[each_dpu]]));
            }
            CPT_XFER(2, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "processed_row_ptr", 0, ALIGN8(max_row_size * sizeof(edge_ptr)), DPU_XFER_DEFAULT)));
        }
        if (max_col_size != 0) {
            DPU_FOREACH(set, dpu, each_dpu) {
                DPU_ASSERT(dpu_prepare_xfer(dpu, &dpu_col_idx[each_dpu][processed_col_size[each_dpu]]));
            }
            CPT_XFER(2, DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "processed_col_idx", 0, ALIGN8(max_col_size * sizeof(node_t)), DPU_XFER_DEFAULT)));
        }
        DPU_FOREACH(set, dpu, each_dpu) {
            processed_row_size[each_dpu] += tmp_row_size[each_dpu];
            processed_col_size[each_dpu] += tmp_col_size[each_dpu];
        }
        start += size;
    }
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        dpu_row_ptr[i][processed_row_size[i]] = processed_col_size[i];
    }

    gettimeofday(&end_time, NULL);
    printf("Mode[2] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_cpt, "Mode[2] Compacting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
#ifdef COMPACT_XFER_PROFILE
    printf("Mode[2] split (ms): exec=%.6f xfer=%.6f host=%.6f\n", m_exec[2], m_xfer[2], get_time_in_milliseconds(&start_time, &end_time) - m_exec[2] - m_xfer[2]);
#endif
#undef CPT_LAUNCH
    total_compacting_time += get_time_in_milliseconds(&start_time, &end_time);
    printf("Total Compacting Time (ms): %.6f\n", total_compacting_time);
    fprintf(fp_time_cpt, "Total Compacting Time (ms): %.6f\n", total_compacting_time);
#else  /* CPU_COMPACTION: build each DPU's compact CSR on the host CPU */
    gettimeofday(&start_time, NULL);
    {
#ifdef CPU_COMPACTION_THREADS
        int n_threads = (int)(CPU_COMPACTION_THREADS);
#else
        int n_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (n_threads < 1) n_threads = 1;
        if ((uint64_t)n_threads > (uint64_t)NR_DPUS) n_threads = (int)NR_DPUS;
        pthread_t *th = malloc((size_t)n_threads * sizeof(pthread_t));
        cpu_compact_arg_t *args = malloc((size_t)n_threads * sizeof(cpu_compact_arg_t));
        if (!th || !args) {
            printf(ANSI_COLOR_RED "Error: cpu compact thread alloc failed\n" ANSI_COLOR_RESET);
            exit(1);
        }
        for (int t = 0; t < n_threads; t++) {
            args[t].tid = (uint32_t)t;
            args[t].n_threads = (uint32_t)n_threads;
            args[t].dpu_row_ptr = dpu_row_ptr;
            args[t].dpu_col_idx = dpu_col_idx;
            args[t].dpu_roots = dpu_roots;
            if (pthread_create(&th[t], NULL, cpu_compact_worker, &args[t]) != 0) {
                printf(ANSI_COLOR_RED "Error: cpu compact pthread_create failed\n" ANSI_COLOR_RESET);
                exit(1);
            }
        }
        for (int t = 0; t < n_threads; t++) pthread_join(th[t], NULL);
        free(th); free(args);
        gettimeofday(&end_time, NULL);
        const double cpu_compacting_time = get_time_in_milliseconds(&start_time, &end_time);
        printf("CPU Compacting Time (ms): %.6f (threads=%d)\n", cpu_compacting_time, n_threads);
        fprintf(fp_time_cpt, "CPU Compacting Time (ms): %.6f (threads=%d)\n", cpu_compacting_time, n_threads);
        printf("Total Compacting Time (ms): %.6f\n", cpu_compacting_time);
        fprintf(fp_time_cpt, "Total Compacting Time (ms): %.6f\n", cpu_compacting_time);
    }
#endif  /* CPU_COMPACTION */
    gettimeofday(&start_time, NULL);

    DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));
    DPU_FOREACH(set, dpu, each_dpu) {
        uint64_t root_num = global_g->root_num[each_dpu];
        DPU_ASSERT(dpu_copy_to(dpu, "root_num", 0, &root_num, sizeof(uint64_t)));
    }
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_roots[each_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "roots", 0, DPU_ROOT_NUM * sizeof(node_t), DPU_XFER_DEFAULT));
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_row_ptr[each_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "row_ptr", 0, DPU_N * sizeof(edge_ptr), DPU_XFER_DEFAULT));
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_col_idx[each_dpu]));
    }
    DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "col_idx", 0, DPU_M * sizeof(node_t), DPU_XFER_DEFAULT));

    gettimeofday(&end_time, NULL);
    printf("DPU data transfer Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_cpt, "DPU data transfer Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fclose(fp_time_cpt);  
    free(dpu_row_ptr);
    free(dpu_col_idx);
    free(dpu_roots);
}

void data_transfer(struct dpu_set_t set, Graph *g) {
    global_g = g;
    struct timeval start_time, end_time;
    FILE *fp_time_tf;   
    
    fp_time_tf = fopen("./result/" "result_"PATTERN_NAME "_" DATA_NAME ".txt", "w");
    if (fp_time_tf == NULL) {
        perror("Error opening file");
        exit(1);
    }
    
    // Measure time for read_input
    gettimeofday(&start_time, NULL);
    
    read_input();
    gettimeofday(&end_time, NULL);
    printf("Data reading Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_tf, "Data reading Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    // Measure time for preprocessing
    
    gettimeofday(&start_time, NULL);
    data_renumber();
    gettimeofday(&end_time, NULL);
    printf("Data renumbering Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_tf, "Data renumbering Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));

    bitmap_t bitmap;   // bitmap of nodes put in dpu
    bitmap = malloc((size_t)(N >> 3) * NR_DPUS);
    
    // Measure time for node allocating
    gettimeofday(&start_time, NULL);
    data_allocate(bitmap);
    gettimeofday(&end_time, NULL);
    printf("Node allocating Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_tf, "Node allocating Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));

#ifdef NO_PARTITION_AS_POSSIBLE
    if (global_g->n > DPU_N - 1 || global_g->m > DPU_M) {
#endif
        data_compact(set, bitmap);
#ifdef NO_PARTITION_AS_POSSIBLE
    }
    else {
        struct dpu_set_t dpu;
        uint32_t each_dpu;
        
        gettimeofday(&start_time, NULL);
        DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));
        node_t max_root_num = 0;
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, &global_g->root_num[each_dpu]));
            if(global_g->root_num[each_dpu] > max_root_num) {
                max_root_num = global_g->root_num[each_dpu];
            }
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->roots[each_dpu]));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "roots", 0, ALIGN8(max_root_num * sizeof(node_t)), DPU_XFER_DEFAULT));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->row_ptr));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "row_ptr", 0, ALIGN8((global_g->n + 1) * sizeof(edge_ptr)), DPU_XFER_DEFAULT));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->col_idx));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "col_idx", 0, ALIGN8(global_g->m * sizeof(node_t)), DPU_XFER_DEFAULT));
        gettimeofday(&end_time, NULL);
        printf("DPU data transfer Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
        fprintf(fp_time_tf, "DPU data transfer Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    }
#endif
    fclose(fp_time_tf);
    free(bitmap);

}

// Multi-round static PimPam: assign workload-sorted roots, build+transfer, launch, collect into result[] over as many rounds as needed.
ans_t  g_pimpam_iter_total       = 0;
double g_pimpam_iter_transfer_ms = 0.0;   /* sum of allocate+compact+transfer */
double g_pimpam_iter_dpu_ms      = 0.0;   /* sum of launch+collect            */
double g_pimpam_iter_exec_ms     = 0.0;   /* sum of dpu_launch (execution)    */
double g_pimpam_iter_collect_ms  = 0.0;   /* sum of answer collection         */
double g_pimpam_iter_assign_ms   = 0.0;   /* sum of data_allocate (task assign) */

void pimpam_iter_run(struct dpu_set_t set, Graph *g) {
    global_g = g;
    struct timeval t0, t1;

    g_pimpam_iter_transfer_ms = 0.0;
    g_pimpam_iter_dpu_ms = 0.0;
    g_pimpam_iter_exec_ms = 0.0;
    g_pimpam_iter_collect_ms = 0.0;
    g_pimpam_iter_assign_ms = 0.0;
    // Count graph load (read + renumber/sort) inside transfer_ms to match OFIS e2e; report read and sort separately.
    gettimeofday(&t0, NULL);
    read_input();
    struct timeval t_read; gettimeofday(&t_read, NULL);
    printf("Data reading Time (ms): %.6f\n", get_time_in_milliseconds(&t0, &t_read));
    data_renumber();
    gettimeofday(&t1, NULL);
    printf("Data Sorting Time (ms): %.6f\n", get_time_in_milliseconds(&t_read, &t1));
    g_pimpam_iter_transfer_ms += get_time_in_milliseconds(&t0, &t1);

    bitmap_t bitmap = malloc((size_t)(N >> 3) * NR_DPUS);
    if (!bitmap) { printf(ANSI_COLOR_RED "Error: PIMPAM_ITER bitmap alloc failed\n" ANSI_COLOR_RESET); exit(1); }

    g_alloc_cursor = 0;
    g_alloc_sorted_done = 0;
    g_pimpam_iter_total = 0;

    struct dpu_set_t dpu;
    uint32_t each_dpu;
    int round = 0;
    do {
        gettimeofday(&t0, NULL);
        data_allocate(bitmap);
        struct timeval t_assign; gettimeofday(&t_assign, NULL);
        g_pimpam_iter_assign_ms += get_time_in_milliseconds(&t0, &t_assign);   /* task assign */
        data_compact(set, bitmap);
        gettimeofday(&t1, NULL);
        g_pimpam_iter_transfer_ms += get_time_in_milliseconds(&t0, &t1);

        gettimeofday(&t0, NULL);
#ifdef PIMPAM_PERDPU_FINISH
#ifdef PIMPAM_PERDPU_SERIAL
        // Serial isolated measurement: launch one DPU at a time synchronously and time its kernel wall in isolation.
        DPU_FOREACH(set, dpu, each_dpu) {
            struct timeval _s, _e;
            gettimeofday(&_s, NULL);
            DPU_ASSERT(dpu_launch(dpu, DPU_SYNCHRONOUS));   /* single-DPU set from DPU_FOREACH */
            gettimeofday(&_e, NULL);
            g_perdpu_finish_ms[each_dpu] += get_time_in_milliseconds(&_s, &_e);
        }
#else
        // Poll measurement: one parallel async launch; a host thread sweeps dpu_status, stamping each DPU's first-observed finish wall.
        {
            DPU_ASSERT(dpu_launch(set, DPU_ASYNCHRONOUS));
            struct timeval t_launch; gettimeofday(&t_launch, NULL);
            static uint8_t _pdone[NR_DPUS];
            memset(_pdone, 0, sizeof(_pdone));
            uint32_t _rem = NR_DPUS;
            while (_rem > 0) {
                DPU_FOREACH(set, dpu, each_dpu) {
                    if (_pdone[each_dpu]) continue;
                    bool _d = false, _f = false;
                    DPU_ASSERT(dpu_status(dpu, &_d, &_f));
                    if (_d) {
                        struct timeval _now; gettimeofday(&_now, NULL);
                        g_perdpu_finish_ms[each_dpu] += get_time_in_milliseconds(&t_launch, &_now);
                        _pdone[each_dpu] = 1; _rem--;
                    }
                }
            }
        }
#endif
#else
        DPU_ASSERT(dpu_launch(set, DPU_SYNCHRONOUS));
#endif
        struct timeval t_exec; gettimeofday(&t_exec, NULL);
        g_pimpam_iter_exec_ms += get_time_in_milliseconds(&t0, &t_exec);   /* execution (dpu_launch) */
        // Answer collection via rank-wide dpu_push_xfer (DPUs stopped): scalar = one value/DPU, per-root = padded to max rn.
#ifdef PIMPAM_COLLECT_PARALLEL
#ifdef OFIS_SCALAR_ANS
        {
            uint64_t *pim_ans = (uint64_t *)malloc((size_t)NR_DPUS * sizeof(uint64_t));
            if (!pim_ans) { perror("pim_ans alloc"); exit(1); }
            DPU_FOREACH(set, dpu, each_dpu)
                DPU_ASSERT(dpu_prepare_xfer(dpu, &pim_ans[each_dpu]));
            DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "ans", 0,
                                     ALIGN8(sizeof(uint64_t)), DPU_XFER_DEFAULT));
            DPU_FOREACH(set, dpu, each_dpu) {
                if (global_g->root_num[each_dpu] == 0) continue;
                result[global_g->roots[each_dpu][0]] = pim_ans[each_dpu];
                g_pimpam_iter_total += pim_ans[each_dpu];
            }
            free(pim_ans);
        }
#else
        {
            uint64_t max_rn = 0;
            DPU_FOREACH(set, dpu, each_dpu)
                if (global_g->root_num[each_dpu] > max_rn) max_rn = global_g->root_num[each_dpu];
            if (max_rn == 0) max_rn = 1;
            uint64_t *pim_ans = (uint64_t *)malloc((size_t)NR_DPUS * max_rn * sizeof(uint64_t));
            if (!pim_ans) { perror("pim_ans alloc"); exit(1); }
            DPU_FOREACH(set, dpu, each_dpu)
                DPU_ASSERT(dpu_prepare_xfer(dpu, pim_ans + (uint64_t)each_dpu * max_rn));
            DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "ans", 0,
                                     ALIGN8(max_rn * sizeof(uint64_t)), DPU_XFER_DEFAULT));
            DPU_FOREACH(set, dpu, each_dpu) {
                const uint64_t rn = global_g->root_num[each_dpu];
                for (node_t k = 0; k < rn; k++) {
                    node_t cur_root = global_g->roots[each_dpu][k];
                    result[cur_root]     = pim_ans[(uint64_t)each_dpu * max_rn + k];
                    g_pimpam_iter_total += pim_ans[(uint64_t)each_dpu * max_rn + k];
                }
            }
            free(pim_ans);
        }
#endif  /* OFIS_SCALAR_ANS */
#else   /* !PIMPAM_COLLECT_PARALLEL: serial per-DPU copy_from */
        DPU_FOREACH(set, dpu, each_dpu) {
            const uint64_t rn = global_g->root_num[each_dpu];
            if (rn == 0) continue;
#ifdef OFIS_SCALAR_ANS
            uint64_t s = 0;
            DPU_ASSERT(dpu_copy_from(dpu, "ans", 0, &s, ALIGN8(sizeof(uint64_t))));
            result[global_g->roots[each_dpu][0]] = s;
            g_pimpam_iter_total += s;
#else
            uint64_t *dpu_ans = (uint64_t *)malloc(ALIGN8(rn * sizeof(uint64_t)));
            DPU_ASSERT(dpu_copy_from(dpu, "ans", 0, dpu_ans, ALIGN8(rn * sizeof(uint64_t))));
            for (node_t k = 0; k < rn; k++) {
                node_t cur_root = global_g->roots[each_dpu][k];
                result[cur_root] = dpu_ans[k];
                g_pimpam_iter_total += dpu_ans[k];
            }
            free(dpu_ans);
#endif  /* OFIS_SCALAR_ANS */
        }
#endif  /* PIMPAM_COLLECT_PARALLEL */
#ifdef PERF
        // Per-DPU cycle load (this round): pull cycle_ct[] and accumulate its per-root sum into cycle_ct_dpu[dpu][0].
        DPU_FOREACH(set, dpu, each_dpu) {
            const uint64_t rn = global_g->root_num[each_dpu];
            if (rn == 0) continue;
            uint64_t *cc = (uint64_t *)malloc(ALIGN8(rn * sizeof(uint64_t)));
            if (!cc) continue;
            DPU_ASSERT(dpu_copy_from(dpu, "cycle_ct", 0, cc, ALIGN8(rn * sizeof(uint64_t))));
            uint64_t s = 0; for (node_t k = 0; k < rn; k++) s += cc[k];
            cycle_ct_dpu[each_dpu][0] += s;
            free(cc);
        }
#endif
        gettimeofday(&t1, NULL);
        g_pimpam_iter_collect_ms += get_time_in_milliseconds(&t_exec, &t1);   /* answer collection */
        g_pimpam_iter_dpu_ms += get_time_in_milliseconds(&t0, &t1);

        round++;
        printf("[PIMPAM_ITER] round %d: %u/%u roots assigned\n",
               round, (unsigned)g_alloc_cursor, (unsigned)global_g->n);
    } while (g_alloc_cursor < global_g->n);
#ifdef PIMPAM_PERDPU_FINISH
    // per-DPU host-observed finish wall (ms), summed over rounds -> DPUK lines.
    for (uint32_t _d = 0; _d < NR_DPUS; ++_d)
        printf("DPUK %u %.3f 0 0 1\n", _d, g_perdpu_finish_ms[_d]);
    fflush(stdout);
#endif

    // Phase breakdown, one "<LABEL> Time (ms): <value>" line per phase.
    printf("Task Assign Time (ms): %.6f\n", g_pimpam_iter_assign_ms);
    printf("DPU Time (ms): %.6f\n", g_pimpam_iter_exec_ms);
    printf("Answer collecting Time (ms): %.6f\n", g_pimpam_iter_collect_ms);

    free(bitmap);
    printf("[PimPam] %d round(s), total ans = %lu\n",
           round, (unsigned long)g_pimpam_iter_total);
}

#ifdef OFIS_PAYLOAD_PATH
// Compact-CSR dispatch for a DPU set (IG/RANK, non-resident): pop a prebuilt batch per masked DPU, push padded to batch max; returns mask served.
static uint64_t compact_dispatch_masked(
    uint32_t thread_id, uint32_t rank_id, struct dpu_set_t rank, uint64_t dpu_mask,
    batch_compact_ctx_t *compact_ctx, compact_payload_t *payloads,
    uint32_t *batch_bitmap, uint8_t *batch_touched_marks,
    uint32_t *batch_touched_words, uint32_t *batch_touched_count,
    uint64_t *root_num_buf, uint64_t *offset_buf, node_t **roots_dst_buf)
{
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    uint64_t active_mask = 0, max_root_count = 0, max_row_bytes = 0, max_col_bytes = 0;
#if defined(OFIS_HYBRID) && !defined(OFIS_LEAN)
    int8_t dpu_region[64];   /* region of each active DPU's batch (by root count); -1 = inactive */
    for (uint32_t i = 0; i < 64; i++) dpu_region[i] = -1;
#endif

    for (uint32_t i = 0; i < 64; i++) {
        payloads[i].valid = 0; payloads[i].root_num = 0; payloads[i].offset = 0;
        payloads[i].row_bytes = 0; payloads[i].col_bytes = 0;
    }

    DPU_FOREACH(rank, dpu, each_dpu) {
        if ((dpu_mask & (1ULL << each_dpu)) == 0) continue;
        node_t   start_node = 0;
        uint32_t my_cnt     = 0;
        compact_acquire_stats_t acq_stats = {0};
        g_pcount_lock_acqs_by_thread[thread_id]++;
        int got = compact_payload_acquire(&payloads[each_dpu], compact_ctx,
                                          batch_bitmap, batch_touched_marks,
                                          batch_touched_words, batch_touched_count,
                                          &start_node, &my_cnt, &acq_stats);
        if (!got) { root_num_buf[each_dpu] = 0; continue; }
#if defined(OFIS_HYBRID) && !defined(OFIS_LEAN)
        { int _ph = g_hybrid_pull_tier; if (_ph < 0 || _ph > 2) _ph = 0;   /* per-tier pop tally */
          if (acq_stats.from_pipeline) g_hybrid_phase_pophit[_ph][thread_id]++;
          else                         g_hybrid_phase_popmiss[_ph][thread_id]++; }
#endif
        if (acq_stats.from_pipeline) {
            g_pipeline_pop_hits_by_thread[thread_id]++;
        } else {
            g_pipeline_pop_misses_by_thread[thread_id]++;
            g_pipeline_inline_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
            g_compact_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
        }

        uint64_t base_idx = global_g->root_num[rank_id * 64 + each_dpu];
        for (uint32_t i = 0; i < my_cnt; i++)
            global_g->roots[rank_id * 64 + each_dpu][base_idx + i] = start_node + i;
        global_g->root_num[rank_id * 64 + each_dpu] += my_cnt;

        uint64_t off_local = global_offset[rank_id][each_dpu];
        global_offset[rank_id][each_dpu] += my_cnt;

        root_num_buf[each_dpu] = my_cnt;
        offset_buf[each_dpu]   = off_local;
        roots_dst_buf[each_dpu] = global_g->roots[rank_id * 64 + each_dpu] + off_local;
        payloads[each_dpu].offset = off_local;
        if (payloads[each_dpu].root_num > max_root_count) max_root_count = payloads[each_dpu].root_num;
        if (payloads[each_dpu].row_bytes > max_row_bytes)  max_row_bytes  = payloads[each_dpu].row_bytes;
        if (payloads[each_dpu].col_bytes > max_col_bytes)  max_col_bytes  = payloads[each_dpu].col_bytes;
        active_mask |= (1ULL << each_dpu);
#if defined(OFIS_HYBRID) && !defined(OFIS_LEAN)
        { int reg = (my_cnt < REGION_CUT1) ? 0 : (my_cnt < REGION_CUT2) ? 1 : 2;   /* region by root count */
          dpu_region[each_dpu] = (int8_t)reg;
          g_region_batches[reg][thread_id]++;
          if (!acq_stats.from_pipeline) g_region_build_ms[reg][thread_id] += acq_stats.inline_build_ms; }
#endif
    }

    if (active_mask == 0) return 0;

    const uint64_t active_ct   = (uint64_t)popcount64(active_mask);
    const uint64_t roots_bytes = ALIGN8(max_root_count * sizeof(node_t));
    struct timeval xfer_t0, xfer_t1;
    gettimeofday(&xfer_t0, NULL);
    for (uint32_t i = 0; i < 64; i++) {
        if (!payloads[i].valid) continue;
        if (ALIGN8(payloads[i].root_num * sizeof(node_t)) < roots_bytes) {
            compact_payload_ensure_root_capacity(&payloads[i], max_root_count);
            memset((uint8_t *)payloads[i].roots + ALIGN8(payloads[i].root_num * sizeof(node_t)), 0,
                   (size_t)(roots_bytes - ALIGN8(payloads[i].root_num * sizeof(node_t))));
        }
        if (payloads[i].row_bytes < max_row_bytes)
            memset((uint8_t *)payloads[i].row_ptr + payloads[i].row_bytes, 0,
                   (size_t)(max_row_bytes - payloads[i].row_bytes));
        if (payloads[i].col_bytes < max_col_bytes)
            memset((uint8_t *)payloads[i].col_idx + payloads[i].col_bytes, 0,
                   (size_t)(max_col_bytes - payloads[i].col_bytes));
    }
    DPU_FOREACH(rank, dpu, each_dpu) {
        if (!(active_mask & (1ULL << each_dpu))) continue;
        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].root_num);
    }
    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    // No-work DPUs get root_num=0 so a re-trigger processes nothing, avoiding a stale-batch re-run race/hang.
    {
        uint64_t no_work_mask = dpu_mask & ~active_mask;
        if (no_work_mask) {
            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(no_work_mask & (1ULL << each_dpu))) continue;
                payloads[each_dpu].root_num = 0;
                OFIS_prepare_xfer_dpu(dpu, no_work_mask, &payloads[each_dpu].root_num);
            }
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
        }
    }
    #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset (prepare+push) */
    DPU_FOREACH(rank, dpu, each_dpu) {
        if (!(active_mask & (1ULL << each_dpu))) continue;
        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].offset);
    }
    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    #endif
    DPU_FOREACH(rank, dpu, each_dpu) {
        if (!(active_mask & (1ULL << each_dpu))) continue;
        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].roots);
    }
    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, roots_bytes);
    DPU_FOREACH(rank, dpu, each_dpu) {
        if (!(active_mask & (1ULL << each_dpu))) continue;
        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].row_ptr);
    }
    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, max_row_bytes);
    /* Prepare only what is pushed: OFIS_push_xfer sends every prepared buffer. */
    if (max_col_bytes > 0) {
        DPU_FOREACH(rank, dpu, each_dpu) {
            if (!(active_mask & (1ULL << each_dpu))) continue;
            OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].col_idx);
        }
        OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, max_col_bytes);
    }
    gettimeofday(&xfer_t1, NULL);
    g_compact_xfer_ms_by_thread[thread_id]     += get_time_in_milliseconds(&xfer_t0, &xfer_t1);
#ifdef GSDPU_BIN_THRESHOLD
    { /* A push carries several DPU-batches: apportion its time by batch count. */
      const double xms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
      uint64_t n[2] = {0, 0};
      for (uint32_t d = 0; d < 64; ++d) {
          if (!(active_mask & (1ULL << d)) || root_num_buf[d] == 0) continue;
          n[GSDPU_BIN(roots_dst_buf[d])]++;
      }
      const uint64_t total = n[0] + n[1];
      for (int b = 0; b < 2 && total > 0; ++b) {
          g_gsxfer_ms[b][thread_id] += xms * (double)n[b] / (double)total;
          g_gsxfer_nb[b][thread_id] += n[b];
      } }
#endif
#ifdef DPU_TIME_PROFILE
    { const double xms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
      for (uint32_t i = 0; i < 64; ++i) if (active_mask & (1ULL << i)) g_dpu_xfer_ms[rank_id*64+i] += xms; }
#endif
    g_compact_batch_count_by_thread[thread_id] += 1;
    g_b_roots_by_thread[thread_id]             += active_ct * roots_bytes;
    g_compact_row_bytes_by_thread[thread_id]   += active_ct * max_row_bytes;
    g_compact_col_bytes_by_thread[thread_id]   += active_ct * max_col_bytes;
#if defined(OFIS_HYBRID) && !defined(OFIS_LEAN)
    {
        const uint64_t per_dpu_bytes = roots_bytes + max_row_bytes + max_col_bytes;
        const double   xfer_ms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
        uint32_t reg_dpus[3] = {0, 0, 0};
        for (uint32_t i = 0; i < 64; i++)
            if ((active_mask & (1ULL << i)) && dpu_region[i] >= 0) reg_dpus[dpu_region[i]]++;
        for (int r = 0; r < 3; r++) {
            if (!reg_dpus[r]) continue;
            g_region_xfer_bytes[r][thread_id] += (uint64_t)reg_dpus[r] * per_dpu_bytes;
            g_region_xfer_ms[r][thread_id]    += xfer_ms * (double)reg_dpus[r] / (double)active_ct;
        }
    }
#endif
    return active_mask;
}
#endif /* OFIS_PAYLOAD_PATH */

void* thread_dpu_fct(void* arg){
    thread_args* a = (thread_args*)arg;
    uint32_t thread_id = a->thread_id;

    uint32_t rank_id = thread_id;

    struct dpu_set_t rank = a->rank;


    cpu_set_t cpu_set; 
    CPU_ZERO(&cpu_set); 
    CPU_SET(thread_id, &cpu_set);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);

    struct dpu_set_t dpu;
    uint32_t each_dpu;

    // Maximum 64 DPUs in one rank
    const uint32_t kDPUsPerRank = 64;

    uint64_t my_dpu_mask = 0xFFFFFFFFFFFFFFFFULL;   
    const uint64_t my_dpu_count = (uint64_t)popcount64(my_dpu_mask);
    batch_compact_ctx_t compact_ctx;
    compact_payload_t *compact_payloads = NULL;
    uint32_t *batch_bitmap = NULL;
    uint8_t *batch_touched_marks = NULL;
    uint32_t *batch_touched_words = NULL;
    uint32_t batch_touched_count = 0;
    if (g_use_batch_compact) {
        const uint32_t n_words = (global_g->n + 31U) >> 5;
        batch_compact_ctx_init(&compact_ctx, global_g->n);
        batch_bitmap = calloc((size_t)n_words, sizeof(uint32_t));
        batch_touched_marks = calloc((size_t)n_words, sizeof(uint8_t));
        batch_touched_words = malloc((size_t)n_words * sizeof(uint32_t));
        compact_payloads = calloc(kDPUsPerRank, sizeof(compact_payload_t));
        if (!batch_bitmap || !batch_touched_marks || !batch_touched_words || !compact_payloads) {
            printf(ANSI_COLOR_RED "Error: compact prefix helper alloc failed\n" ANSI_COLOR_RESET);
            exit(1);
        }
        compact_payloads_init(compact_payloads, kDPUsPerRank);
    }

#ifdef OFIS_THREAD_INIT_CSR_XFER
    if (!g_use_batch_compact) {
        const uint32_t kDPUsPerIG = 8;
        struct timeval init_t0, init_t1;
        const uint32_t row_ptr_bytes = ALIGN8((global_g->n + 1) * sizeof(edge_ptr));
        const uint32_t col_idx_bytes = ALIGN8(global_g->m * sizeof(node_t));

        gettimeofday(&init_t0, NULL);
        DPU_FOREACH(rank, dpu, each_dpu) {
            uint32_t ig_id = each_dpu % kDPUsPerIG;
            OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
            OFIS_prepare_xfer_dpu(dpu, my_dpu_mask, global_g->row_ptr);
        }
        OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, row_ptr_bytes);
        gettimeofday(&init_t1, NULL);
        g_init_rowptr_ms_by_thread[thread_id] += get_time_in_milliseconds(&init_t0, &init_t1);

        gettimeofday(&init_t0, NULL);
        DPU_FOREACH(rank, dpu, each_dpu) {
            uint32_t ig_id = each_dpu % kDPUsPerIG;
            OFIS_prepare_xfer_dpu(dpu, my_dpu_mask, global_g->col_idx);
        }
        OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, col_idx_bytes);
        gettimeofday(&init_t1, NULL);
        g_init_colidx_ms_by_thread[thread_id] += get_time_in_milliseconds(&init_t0, &init_t1);

    }
#endif

    // temporal buffer initialization
    uint64_t *root_num_buf = (uint64_t*)malloc(sizeof(uint64_t) * kDPUsPerRank);
    uint64_t *offset_buf = (uint64_t*)malloc(sizeof(uint64_t) * kDPUsPerRank);
    node_t **roots_dst_buf = (node_t**)malloc(sizeof(node_t*) * kDPUsPerRank); // for roots buffer pointer
    node_t **initial_roots_payloads = (node_t**)calloc(kDPUsPerRank, sizeof(node_t*));

#ifdef OFIS_COLLECT_PER_BATCH
    // Collect one round BEHIND: on finishing batch k, read k-1 (k may still be settling); last batch drained after the loop.
    uint64_t pb_prev_off[64] = {0};
    uint64_t pb_prev_cnt[64] = {0};
    uint64_t pb_last_off[64] = {0};
    uint64_t pb_last_cnt[64] = {0};
#endif

    if (!root_num_buf || !offset_buf || !roots_dst_buf || !initial_roots_payloads) {
        printf(ANSI_COLOR_RED "Error: initial OFIS buffer alloc failed\n" ANSI_COLOR_RESET);
        exit(1);
    }


    uint64_t cnt = 0, off = 0;

    uint64_t running_dpu_mask = 0;
    // initialize batch
    struct timeval assign_t0, assign_t1;
    gettimeofday(&assign_t0, NULL);

#ifdef OFIS_PAYLOAD_PATH
    if (g_use_batch_compact && ofis_payload_path_enabled()) {
        // Pipeline path: per DPU, pop a prebuilt payload or fall back to inline build.
        compact_payload_t *payloads = compact_payloads;
        uint64_t active_mask = 0;
        uint64_t max_root_count = 0;
        uint64_t max_row_bytes = 0;
        uint64_t max_col_bytes = 0;
        struct timeval xfer_t0, xfer_t1;

        for (uint32_t i = 0; i < kDPUsPerRank; i++) {
            payloads[i].valid = 0;
            payloads[i].root_num = 0;
            payloads[i].offset = 0;
            payloads[i].row_bytes = 0;
            payloads[i].col_bytes = 0;
            root_num_buf[i] = 0;
            offset_buf[i] = 0;
        }

        DPU_FOREACH(rank, dpu, each_dpu) {
            uint32_t ig_id = each_dpu % 8;
            if ((my_dpu_mask & (1ULL << each_dpu)) == 0) continue;

            node_t  start_node = 0;
            uint32_t my_cnt    = 0;
            compact_acquire_stats_t acq_stats = {0};

            g_pcount_lock_acqs_by_thread[thread_id]++;
            struct timeval acq_t0, acq_t1;
            gettimeofday(&acq_t0, NULL);
            int got = compact_payload_acquire(&payloads[each_dpu],
                                              &compact_ctx,
                                              batch_bitmap,
                                              batch_touched_marks,
                                              batch_touched_words,
                                              &batch_touched_count,
                                              &start_node,
                                              &my_cnt,
                                              &acq_stats);
            gettimeofday(&acq_t1, NULL);
            const double acq_ms = get_time_in_milliseconds(&acq_t0, &acq_t1);
            g_pipeline_acquire_ms_by_thread[thread_id] += acq_ms;
            if (got) {
                g_init_build_sum_ms_by_thread[thread_id] += acq_ms;
                if (acq_ms > g_init_build_max_ms_by_thread[thread_id])
                    g_init_build_max_ms_by_thread[thread_id] = acq_ms;
                g_init_dpu_cnt_by_thread[thread_id]++;
                if (!acq_stats.from_pipeline) g_init_inline_cnt_by_thread[thread_id]++;
                if (acq_stats.from_pipeline) {
                    g_pipeline_pop_hits_by_thread[thread_id]++;
                } else {
                    g_pipeline_pop_misses_by_thread[thread_id]++;
                    g_pipeline_inline_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
                    g_compact_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
                }
            } else {
                continue;
            }
            running_dpu_mask |= (1ULL << each_dpu);

            struct timeval bk_t0, bk_t1;
            gettimeofday(&bk_t0, NULL);
            uint64_t base_idx = global_g->root_num[rank_id * 64 + each_dpu];
            for (uint32_t i = 0; i < my_cnt; i++) {
                global_g->roots[rank_id * 64 + each_dpu][base_idx + i] = start_node + i;
            }
            global_g->root_num[rank_id * 64 + each_dpu] += my_cnt;

            uint64_t off_local = global_offset[rank_id][each_dpu];
            global_offset[rank_id][each_dpu] += my_cnt;

            root_num_buf[each_dpu] = my_cnt;
            offset_buf[each_dpu]   = off_local;
#ifdef OFIS_COLLECT_PER_BATCH
            pb_last_off[each_dpu] = off_local;
            pb_last_cnt[each_dpu] = my_cnt;
#endif
            roots_dst_buf[each_dpu] =
                global_g->roots[rank_id * 64 + each_dpu] + off_local;

            payloads[each_dpu].offset = off_local;
            if (payloads[each_dpu].root_num > max_root_count) max_root_count = payloads[each_dpu].root_num;
            if (payloads[each_dpu].row_bytes > max_row_bytes) max_row_bytes = payloads[each_dpu].row_bytes;
            if (payloads[each_dpu].col_bytes > max_col_bytes) max_col_bytes = payloads[each_dpu].col_bytes;
            active_mask |= (1ULL << each_dpu);
            gettimeofday(&bk_t1, NULL);
            g_assign_ms_by_thread[thread_id] += get_time_in_milliseconds(&bk_t0, &bk_t1);
        }

        if (active_mask != 0) {
            const uint64_t active_ct  = (uint64_t)popcount64(active_mask);
            const uint64_t roots_bytes = ALIGN8(max_root_count * sizeof(node_t));
            gettimeofday(&xfer_t0, NULL);
            for (uint32_t i = 0; i < 64; i++) {
                if (!payloads[i].valid) continue;
                if (ALIGN8(payloads[i].root_num * sizeof(node_t)) < roots_bytes) {
                    compact_payload_ensure_root_capacity(&payloads[i], max_root_count);
                    memset((uint8_t *)payloads[i].roots + ALIGN8(payloads[i].root_num * sizeof(node_t)), 0,
                           (size_t)(roots_bytes - ALIGN8(payloads[i].root_num * sizeof(node_t))));
                }
                if (payloads[i].row_bytes < max_row_bytes) {
                    memset((uint8_t *)payloads[i].row_ptr + payloads[i].row_bytes, 0,
                           (size_t)(max_row_bytes - payloads[i].row_bytes));
                }
                if (payloads[i].col_bytes < max_col_bytes) {
                    memset((uint8_t *)payloads[i].col_idx + payloads[i].col_bytes, 0,
                           (size_t)(max_col_bytes - payloads[i].col_bytes));
                }
            }

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
                OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].root_num);
            }
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].offset);
            }
            #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
            #endif

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].roots);
            }
            OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, roots_bytes);

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].row_ptr);
            }
            OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, max_row_bytes);

            /* Prepare only what is pushed: OFIS_push_xfer sends every prepared buffer. */
            if (max_col_bytes > 0) {
                DPU_FOREACH(rank, dpu, each_dpu) {
                    if (!(active_mask & (1ULL << each_dpu))) continue;
                    OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].col_idx);
                }
                OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, max_col_bytes);
            }
            gettimeofday(&xfer_t1, NULL);
            const double this_xfer_ms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
            g_compact_xfer_ms_by_thread[thread_id] += this_xfer_ms;
            if (g_init_transfer_ms_by_thread[thread_id] == 0.0)
                g_init_transfer_ms_by_thread[thread_id] = this_xfer_ms;   /* first transfer only */
            g_compact_batch_count_by_thread[thread_id]++;
            g_b_roots_by_thread[thread_id] += active_ct * roots_bytes;
            g_compact_row_bytes_by_thread[thread_id] += active_ct * max_row_bytes;
            g_compact_col_bytes_by_thread[thread_id] += active_ct * max_col_bytes;
        }

        g_t_rootnum_ms_by_thread[thread_id] += 0.0;
        g_t_offset_ms_by_thread[thread_id]  += 0.0;
        g_t_roots_ms_by_thread[thread_id]   += 0.0;
        goto initial_transfer_done;
    }
#endif /* OFIS_PAYLOAD_PATH */

    DPU_FOREACH(rank, dpu, each_dpu) {
        uint32_t ig_id = each_dpu % 8;
        struct timeval l0, l_done;
        gettimeofday(&l0, NULL);

        node_t start_node = 0;
        uint64_t my_cnt = 0;

        g_pcount_lock_acqs_by_thread[thread_id]++;
        for (;;) {
            uint32_t snapshot = __atomic_load_n(&p_count, __ATOMIC_ACQUIRE);
            if (snapshot >= g_total_roots) {
                cnt = 0;
                break;
            }
            uint64_t remain = g_total_roots - snapshot;

            node_t cur_node = snapshot;
            edge_ptr degree = global_g->row_ptr[cur_node + 1] - global_g->row_ptr[cur_node];

            uint32_t current_batch_size = g_auto_batch_size;
            if(degree >= BATCH_HEAVY_THRESHOLD){
                current_batch_size = g_auto_batch_size / HEAVY_NODE_DIVISOR;
                if(current_batch_size == 0) current_batch_size = 1;
            }

            cnt = (remain > current_batch_size) ? current_batch_size : (uint32_t)remain;
            if (g_use_batch_compact) {
                edge_ptr estimated_local_m = 0;
                cnt = find_compact_safe_prefix_count(
                    cur_node,
                    (uint32_t)cnt,
                    batch_bitmap,
                    batch_touched_marks,
                    batch_touched_words,
                    &batch_touched_count,
                    &estimated_local_m
                );
                if (cnt == 0) {
                    printf(ANSI_COLOR_RED
                           "Error: root %u alone exceeds compact DPU_M bound (degree=%u, estimated local_m>%u)\n"
                           ANSI_COLOR_RESET,
                           cur_node, degree, (unsigned)DPU_M);
                    exit(1);
                }
            }

            uint32_t expected = snapshot;
            uint32_t desired = snapshot + (uint32_t)cnt;
            if (__atomic_compare_exchange_n(&p_count, &expected, desired,
                                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                start_node = snapshot;
                my_cnt = cnt;
                running_dpu_mask |= (1ULL << each_dpu);
                break;
            }
            // CAS failed: another thread reserved this range; retry with fresh snapshot.
        }
        gettimeofday(&l_done, NULL);
        g_pcount_cs_hold_ms_by_thread[thread_id] += get_time_in_milliseconds(&l0, &l_done);

        // roots[]/root_num[] slot is keyed by (rank_id, each_dpu); only this thread writes it
        // (different threads of the same rank handle disjoint IG ranges).
        if(cnt > 0) {
            uint64_t base_idx = global_g->root_num[rank_id * 64 + each_dpu];
            for(int i = 0; i < cnt; i++){
                global_g->roots[rank_id * 64 + each_dpu][base_idx + i] = start_node + i;
            }
            global_g->root_num[rank_id * 64 + each_dpu] += cnt;
        }

        off = global_offset[rank_id][each_dpu];
        global_offset[rank_id][each_dpu] += cnt;

        root_num_buf[each_dpu] = cnt;
        offset_buf[each_dpu] = off;
#ifdef OFIS_COLLECT_PER_BATCH
        pb_last_off[each_dpu] = off;
        pb_last_cnt[each_dpu] = cnt;
#endif
        node_t *src_ptr = global_g->roots[rank_id * 64 + each_dpu] + off;
        roots_dst_buf[each_dpu] = src_ptr;
        (void)my_cnt;
    }
    gettimeofday(&assign_t1, NULL);
    g_assign_ms_by_thread[thread_id] += get_time_in_milliseconds(&assign_t0, &assign_t1);
    struct timeval t0, t1;
    if (g_use_batch_compact) {
        compact_payload_t *payloads = compact_payloads;
        uint64_t active_mask = 0;
        uint64_t max_root_count = 0;
        uint64_t max_row_bytes = 0;
        uint64_t max_col_bytes = 0;
        struct timeval cpt_t0, cpt_t1, xfer_t0, xfer_t1;
        gettimeofday(&cpt_t0, NULL);

        for (uint32_t i = 0; i < kDPUsPerRank; i++) {
            payloads[i].valid = 0;
            payloads[i].root_num = 0;
            payloads[i].offset = 0;
            payloads[i].row_bytes = 0;
            payloads[i].col_bytes = 0;
        }

        DPU_FOREACH(rank, dpu, each_dpu) {
            uint32_t ig_id = each_dpu % 8;
            if ((my_dpu_mask & (1ULL << each_dpu)) == 0) continue;
            if (root_num_buf[each_dpu] == 0) continue;

            uint64_t local_n = 0, local_m = 0;
            compact_payload_t *pl = &payloads[each_dpu];
            batch_compact_build(&compact_ctx, roots_dst_buf[each_dpu], root_num_buf[each_dpu], &local_n, &local_m);
            pl->valid = 1;
            pl->root_num = root_num_buf[each_dpu];
            pl->offset = offset_buf[each_dpu];
            pl->row_bytes = ALIGN8((local_n + 1) * sizeof(edge_ptr));
            pl->col_bytes = ALIGN8(local_m * sizeof(node_t));
            if (pl->root_num > max_root_count) max_root_count = pl->root_num;
            compact_payload_ensure_root_capacity(pl, pl->root_num);
            memcpy(pl->roots, compact_ctx.local_roots, (size_t)pl->root_num * sizeof(node_t));
            memcpy(pl->row_ptr, compact_ctx.local_row_ptr, pl->row_bytes);
            if (pl->col_bytes) {
                memcpy(pl->col_idx, compact_ctx.local_col_idx, pl->col_bytes);
            }
            if (pl->row_bytes > max_row_bytes) max_row_bytes = pl->row_bytes;
            if (pl->col_bytes > max_col_bytes) max_col_bytes = pl->col_bytes;
            active_mask |= (1ULL << each_dpu);
        }
        gettimeofday(&cpt_t1, NULL);
        g_compact_build_ms_by_thread[thread_id] += get_time_in_milliseconds(&cpt_t0, &cpt_t1);

        if (active_mask != 0) {
            const uint64_t active_ct = (uint64_t)popcount64(active_mask);
            const uint64_t roots_bytes = ALIGN8(max_root_count * sizeof(node_t));
            gettimeofday(&xfer_t0, NULL);
            for (uint32_t i = 0; i < 64; i++) {
                if (!payloads[i].valid) continue;
                if (ALIGN8(payloads[i].root_num * sizeof(node_t)) < roots_bytes) {
                    compact_payload_ensure_root_capacity(&payloads[i], max_root_count);
                    memset((uint8_t *)payloads[i].roots + ALIGN8(payloads[i].root_num * sizeof(node_t)), 0,
                           (size_t)(roots_bytes - ALIGN8(payloads[i].root_num * sizeof(node_t))));
                }
                if (payloads[i].row_bytes < max_row_bytes) {
                    memset((uint8_t *)payloads[i].row_ptr + payloads[i].row_bytes, 0,
                           (size_t)(max_row_bytes - payloads[i].row_bytes));
                }
                if (payloads[i].col_bytes < max_col_bytes) {
                    memset((uint8_t *)payloads[i].col_idx + payloads[i].col_bytes, 0,
                           (size_t)(max_col_bytes - payloads[i].col_bytes));
                }
            }

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
                OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].root_num);
            }
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].offset);
            }
            #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
            #endif

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].roots);
            }
            OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, roots_bytes);

            DPU_FOREACH(rank, dpu, each_dpu) {
                if (!(active_mask & (1ULL << each_dpu))) continue;
                OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].row_ptr);
            }
            OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, max_row_bytes);

            /* Prepare only what is pushed: OFIS_push_xfer sends every prepared buffer. */
            if (max_col_bytes > 0) {
                DPU_FOREACH(rank, dpu, each_dpu) {
                    if (!(active_mask & (1ULL << each_dpu))) continue;
                    OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].col_idx);
                }
                OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, max_col_bytes);
            }
            gettimeofday(&xfer_t1, NULL);
            const double this_xfer_ms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
            g_compact_xfer_ms_by_thread[thread_id] += this_xfer_ms;
            if (g_init_transfer_ms_by_thread[thread_id] == 0.0)
                g_init_transfer_ms_by_thread[thread_id] = this_xfer_ms;   /* first transfer only */
            g_compact_batch_count_by_thread[thread_id]++;
            g_b_roots_by_thread[thread_id] += active_ct * roots_bytes;
            g_compact_row_bytes_by_thread[thread_id] += active_ct * max_row_bytes;
            g_compact_col_bytes_by_thread[thread_id] += active_ct * max_col_bytes;
        }

        g_t_rootnum_ms_by_thread[thread_id] += 0.0;
        g_t_offset_ms_by_thread[thread_id] += 0.0;
        g_t_roots_ms_by_thread[thread_id] += 0.0;
        goto initial_transfer_done;
    }

    uint64_t initial_active_mask = 0;
    uint64_t initial_max_root_num = 0;

    DPU_FOREACH(rank, dpu, each_dpu) {
        uint32_t ig_id = each_dpu % 8;
        if (root_num_buf[each_dpu] == 0) continue;
        initial_active_mask |= (1ULL << each_dpu);
        if (root_num_buf[each_dpu] > initial_max_root_num) {
            initial_max_root_num = root_num_buf[each_dpu];
        }
        initial_roots_payloads[each_dpu] = calloc(DPU_ROOT_NUM, sizeof(node_t));
        if (!initial_roots_payloads[each_dpu]) {
            printf(ANSI_COLOR_RED "Error: initial roots staging alloc failed for DPU %u\n" ANSI_COLOR_RESET, each_dpu);
            exit(1);
        }
        memcpy(initial_roots_payloads[each_dpu], roots_dst_buf[each_dpu],
               (size_t)root_num_buf[each_dpu] * sizeof(node_t));
    }
    const uint64_t initial_roots_bytes = ALIGN8(initial_max_root_num * sizeof(node_t));

    // send root_num to each batch
    gettimeofday(&t0, NULL);
    
    DPU_FOREACH(rank, dpu, each_dpu) {
        uint32_t ig_id = each_dpu % 8;
        OFIS_prepare_xfer_dpu(dpu, initial_active_mask, &root_num_buf[each_dpu]);
    }
    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    gettimeofday(&t1, NULL);
    g_t_rootnum_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    g_c_rootnum_by_thread[thread_id]++;
    g_n_rootnum_dpu_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask);
    g_b_rootnum_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask) * sizeof(uint64_t);

    // send offset to each batch
    gettimeofday(&t0, NULL);
    
    DPU_FOREACH(rank, dpu, each_dpu) {
        uint32_t ig_id = each_dpu % 8;
        OFIS_prepare_xfer_dpu(dpu, initial_active_mask, &offset_buf[each_dpu]);
    }
    #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
    #endif
    gettimeofday(&t1, NULL);
    g_t_offset_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    g_c_offset_by_thread[thread_id]++;
    g_n_offset_dpu_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask);
    g_b_offset_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask) * sizeof(uint64_t);
    
    gettimeofday(&t0, NULL);
    
    DPU_FOREACH(rank, dpu, each_dpu) {
        uint32_t ig_id = each_dpu % 8;
        if ((initial_active_mask & (1ULL << each_dpu)) == 0) continue;
        OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
        OFIS_prepare_xfer_dpu(dpu, initial_active_mask, initial_roots_payloads[each_dpu]);
    }
    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, initial_roots_bytes);
    gettimeofday(&t1, NULL);
    g_t_roots_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
    g_c_roots_by_thread[thread_id]++;
    g_n_roots_dpu_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask);
    g_b_roots_by_thread[thread_id] += (uint64_t)popcount64(initial_active_mask) * initial_roots_bytes;

initial_transfer_done:
{
    OFIS_STARTUP_SYNC();               /* no-op unless OFIS_STARTUP_BARRIER */
    OFIS_dpu_launch(rank);
    OFIS_STAMP_FIRST_LAUNCH(thread_id);
}

    int finish = 0;
    uint64_t finish_dpu = 0;
    uint64_t loop_iter = 0;
    double compact_build_ms = 0.0;
    double compact_xfer_ms = 0.0;
    double wait_finish_ms = 0.0;
    struct timeval loop_start_ts, last_progress_ts;
    gettimeofday(&loop_start_ts, NULL);
    last_progress_ts = loop_start_ts;

    // timer variable
    struct timeval t_now;

    // time recording variable
    struct timeval dpu_last_timestamp[64];

    // initialize all dpu's timestamp
    gettimeofday(&t_now, NULL);
    for (int i = 0; i < 64; i++) {
        dpu_last_timestamp[i] = t_now;
    }
    dpu_dbg_store(thread_id, DPU_DBG_STAGE_INIT, loop_iter, running_dpu_mask, 0, 0);
    __atomic_store_n(&g_dpu_dbg[thread_id].done, 0, __ATOMIC_RELAXED);

    while (!finish) {
        dpu_dbg_store(thread_id, DPU_DBG_STAGE_WAIT_FINISH, loop_iter, running_dpu_mask, finish_dpu, 0);
        if (running_dpu_mask == 0) {
            finish = 1;
            break;
        }
        loop_iter++;
        // wait for current rank finish
        uint32_t nr_finish_dpu = 0;
        const uint64_t wait_begin_ms = dbg_now_ms();
        // start measure waiting time
        struct timeval wait_t0, wait_t1;
        gettimeofday(&wait_t0, NULL);
        do {
            nr_finish_dpu = OFIS_get_finished_dpu(rank, &finish_dpu);
            if (running_dpu_mask == 0) {
                finish = 1;
                break;
            }
            if (nr_finish_dpu == 0) POLL_BACKOFF();
        } while (nr_finish_dpu == 0);
        if (finish) {
            break;
        }
        dpu_dbg_store(thread_id, DPU_DBG_STAGE_GOT_FINISH, loop_iter, running_dpu_mask, finish_dpu, 0);
        gettimeofday(&wait_t1, NULL);
        g_wait_ms_by_thread[thread_id] += get_time_in_milliseconds(&wait_t0, &wait_t1);
        wait_finish_ms += get_time_in_milliseconds(&wait_t0, &wait_t1);
        gettimeofday(&t_now, NULL);
        // Set MUX for the CPU (To Read & Write data)
        gettimeofday(&assign_t0, NULL);
        for(int i = 0; i < 64; ++i){
            uint32_t ig_id = i % 8;
            if(finish_dpu & (1ULL << i)){
                double cycle_ms = get_time_in_milliseconds(&dpu_last_timestamp[i], &t_now);
                // record DPU's cycle time
                g_dpu_cycle_sum[thread_id][i] += cycle_ms;
                g_dpu_cycle_count[thread_id][i]++;
                
                // renew DPU's last timestamp
                dpu_last_timestamp[i] = t_now;
                OFIS_set_mux_dpu(rank, i, OPEN_TO_HOST);
            }
        }
        gettimeofday(&assign_t1, NULL);
        g_mux_host_ms_by_thread[thread_id] += get_time_in_milliseconds(&assign_t0, &assign_t1);

#ifdef OFIS_COLLECT_PER_BATCH
        // Drain the batch BEFORE the one just finished (one-round lag).
#ifdef OFIS_COLLECT_NO_LAG
        ofis_collect_batch_slice(thread_id, rank_id, rank,
                                 finish_dpu & running_dpu_mask,
                                 pb_last_off, pb_last_cnt);
#else
        ofis_collect_batch_slice(thread_id, rank_id, rank,
                                 finish_dpu & running_dpu_mask,
                                 pb_prev_off, pb_prev_cnt);
#endif
#endif

        uint32_t any_assigned = 0;
        uint64_t assigned_dpu = 0;
        uint64_t cnt = 0, off = 0;
        uint64_t max_cnt = 0;
        dpu_dbg_store(thread_id, DPU_DBG_STAGE_ASSIGN, loop_iter, running_dpu_mask, finish_dpu, assigned_dpu);
        gettimeofday(&assign_t0, NULL);

#ifdef OFIS_PAYLOAD_PATH
        if (g_use_batch_compact && ofis_payload_path_enabled()) {
            // Pipeline refill: per finished DPU, pop a prebuilt payload or build inline.
            compact_payload_t *payloads = compact_payloads;
            uint64_t active_mask = 0;
            uint64_t max_root_count = 0;
            uint64_t max_row_bytes = 0;
            uint64_t max_col_bytes = 0;
            struct timeval xfer_t0, xfer_t1;

            for (uint32_t i = 0; i < kDPUsPerRank; i++) {
                payloads[i].valid = 0;
                payloads[i].root_num = 0;
                payloads[i].offset = 0;
                payloads[i].row_bytes = 0;
                payloads[i].col_bytes = 0;
            }

            DPU_FOREACH(rank, dpu, each_dpu) {
                uint32_t ig_id = each_dpu % 8;
                if (!((finish_dpu & (1ULL << each_dpu)) &&
                      (running_dpu_mask & (1ULL << each_dpu)))) continue;

                running_dpu_mask &= ~(1ULL << each_dpu);

                node_t  start_node = 0;
                uint32_t my_cnt    = 0;
                compact_acquire_stats_t acq_stats = {0};

                g_pcount_lock_acqs_by_thread[thread_id]++;
                struct timeval acq_t0, acq_t1;
                gettimeofday(&acq_t0, NULL);
                int got = compact_payload_acquire(&payloads[each_dpu],
                                                  &compact_ctx,
                                                  batch_bitmap,
                                                  batch_touched_marks,
                                                  batch_touched_words,
                                                  &batch_touched_count,
                                                  &start_node,
                                                  &my_cnt,
                                                  &acq_stats);
                gettimeofday(&acq_t1, NULL);
                const double acq_ms = get_time_in_milliseconds(&acq_t0, &acq_t1);
                g_pipeline_acquire_ms_by_thread[thread_id] += acq_ms;
                if (got) {
                    if (acq_stats.from_pipeline) {
                        g_pipeline_pop_hits_by_thread[thread_id]++;
                    } else {
                        g_pipeline_pop_misses_by_thread[thread_id]++;
                        g_pipeline_inline_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
                        g_compact_build_ms_by_thread[thread_id] += acq_stats.inline_build_ms;
                        compact_build_ms += acq_stats.inline_build_ms;
                    }
                } else {
                    root_num_buf[each_dpu] = 0;
                    offset_buf[each_dpu]   = 0;
                    continue;
                }
                running_dpu_mask |= (1ULL << each_dpu);
                assigned_dpu     |= (1ULL << each_dpu);
                any_assigned = 1;

                struct timeval bk_t0, bk_t1;
                gettimeofday(&bk_t0, NULL);
                uint64_t base_idx = global_g->root_num[rank_id * 64 + each_dpu];
                for (uint32_t i = 0; i < my_cnt; i++) {
                    global_g->roots[rank_id * 64 + each_dpu][base_idx + i] = start_node + i;
                }
                global_g->root_num[rank_id * 64 + each_dpu] += my_cnt;
                if (my_cnt > max_cnt) max_cnt = my_cnt;

                uint64_t off_local = global_offset[rank_id][each_dpu];
                global_offset[rank_id][each_dpu] += my_cnt;

                root_num_buf[each_dpu] = my_cnt;
                offset_buf[each_dpu]   = off_local;
#ifdef OFIS_COLLECT_PER_BATCH
                // Only a real batch advances pb_last (its last executed batch).
                if (my_cnt > 0) {
                    pb_prev_off[each_dpu] = pb_last_off[each_dpu];
                    pb_prev_cnt[each_dpu] = pb_last_cnt[each_dpu];
                    pb_last_off[each_dpu] = off_local;
                    pb_last_cnt[each_dpu] = my_cnt;
                }
#endif
                g_last_rootnum_by_thread[thread_id][each_dpu] = my_cnt;
                g_last_offset_by_thread[thread_id][each_dpu]  = off_local;
                g_last_start_root_by_thread[thread_id][each_dpu] = start_node;
                g_last_assign_iter_by_thread[thread_id][each_dpu] = loop_iter;
                roots_dst_buf[each_dpu] =
                    global_g->roots[rank_id * 64 + each_dpu] + off_local;

                payloads[each_dpu].offset = off_local;
                if (payloads[each_dpu].root_num > max_root_count) max_root_count = payloads[each_dpu].root_num;
                if (payloads[each_dpu].row_bytes > max_row_bytes) max_row_bytes = payloads[each_dpu].row_bytes;
                if (payloads[each_dpu].col_bytes > max_col_bytes) max_col_bytes = payloads[each_dpu].col_bytes;
                active_mask |= (1ULL << each_dpu);
                gettimeofday(&bk_t1, NULL);
                g_assign_ms_by_thread[thread_id] += get_time_in_milliseconds(&bk_t0, &bk_t1);
            }

            if (any_assigned) {
                if (active_mask != 0) {
                    const uint64_t active_ct  = (uint64_t)popcount64(active_mask);
                    const uint64_t roots_bytes = ALIGN8(max_root_count * sizeof(node_t));
                    gettimeofday(&xfer_t0, NULL);
                    for (uint32_t i = 0; i < 64; i++) {
                        if (!payloads[i].valid) continue;
                        if (ALIGN8(payloads[i].root_num * sizeof(node_t)) < roots_bytes) {
                            compact_payload_ensure_root_capacity(&payloads[i], max_root_count);
                            memset((uint8_t *)payloads[i].roots + ALIGN8(payloads[i].root_num * sizeof(node_t)), 0,
                                   (size_t)(roots_bytes - ALIGN8(payloads[i].root_num * sizeof(node_t))));
                        }
                        if (payloads[i].row_bytes < max_row_bytes) {
                            memset((uint8_t *)payloads[i].row_ptr + payloads[i].row_bytes, 0,
                                   (size_t)(max_row_bytes - payloads[i].row_bytes));
                        }
                        if (payloads[i].col_bytes < max_col_bytes) {
                            memset((uint8_t *)payloads[i].col_idx + payloads[i].col_bytes, 0,
                                   (size_t)(max_col_bytes - payloads[i].col_bytes));
                        }
                    }

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
                        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].root_num);
                    }
                    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].offset);
                    }
                    #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
                    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
                    #endif

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].roots);
                    }
                    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, roots_bytes);

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].row_ptr);
                    }
                    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, max_row_bytes);

                    /* Prepare only what is pushed: OFIS_push_xfer sends every prepared buffer. */
                    if (max_col_bytes > 0) {
                        DPU_FOREACH(rank, dpu, each_dpu) {
                            if (!(active_mask & (1ULL << each_dpu))) continue;
                            OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].col_idx);
                        }
                        OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, max_col_bytes);
                    }
                    gettimeofday(&xfer_t1, NULL);
                    {
                        const double xfer_ms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
                        compact_xfer_ms += xfer_ms;
                        g_compact_xfer_ms_by_thread[thread_id] += xfer_ms;
                    }
                    g_b_roots_by_thread[thread_id] += active_ct * roots_bytes;
                    g_compact_batch_count_by_thread[thread_id]++;
                    g_compact_row_bytes_by_thread[thread_id] += active_ct * max_row_bytes;
                    g_compact_col_bytes_by_thread[thread_id] += active_ct * max_col_bytes;
                }
            }
            goto dynamic_transfer_done;
        }
#endif /* OFIS_PAYLOAD_PATH */

        DPU_FOREACH(rank, dpu, each_dpu) {
            uint32_t ig_id = each_dpu % 8;
            // check current dpu is involved in 'finished' dpu
            if((finish_dpu & (1ULL << each_dpu)) && (running_dpu_mask & (1ULL << each_dpu))){
                // for only finished dpu
                running_dpu_mask &= ~(1ULL << each_dpu);
                cnt = 0;

                node_t start_node = 0;
                uint64_t my_cnt = 0;

                struct timeval l3, l_done;
                gettimeofday(&l3, NULL);
                g_pcount_lock_acqs_by_thread[thread_id]++;

                for (;;) {
                    uint32_t snapshot = __atomic_load_n(&p_count, __ATOMIC_ACQUIRE);
                    if (snapshot >= g_total_roots) {
                        cnt = 0;
                        break;
                    }
                    uint64_t remain = g_total_roots - snapshot;

                    node_t cur_node = snapshot;
                    edge_ptr degree = global_g->row_ptr[cur_node + 1] - global_g->row_ptr[cur_node];

                    uint32_t current_batch_size = g_auto_batch_size;
                    if(degree >= BATCH_HEAVY_THRESHOLD){
                        current_batch_size = g_auto_batch_size / HEAVY_NODE_DIVISOR;
                        if(current_batch_size == 0) current_batch_size = 1;
                    }

                    cnt = (remain > current_batch_size) ? current_batch_size : (uint32_t)remain;
                    if (g_use_batch_compact) {
                        edge_ptr estimated_local_m = 0;
                        cnt = find_compact_safe_prefix_count(
                            cur_node,
                            (uint32_t)cnt,
                            batch_bitmap,
                            batch_touched_marks,
                            batch_touched_words,
                            &batch_touched_count,
                            &estimated_local_m
                        );
                        if (cnt == 0) {
                            printf(ANSI_COLOR_RED
                                   "Error: root %u alone exceeds compact DPU_M bound (degree=%u, estimated local_m>%u)\n"
                                   ANSI_COLOR_RESET,
                                   cur_node, degree, (unsigned)DPU_M);
                            exit(1);
                        }
                    }

                    uint32_t expected = snapshot;
                    uint32_t desired = snapshot + (uint32_t)cnt;
                    if (__atomic_compare_exchange_n(&p_count, &expected, desired,
                                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        start_node = snapshot;
                        my_cnt = cnt;
                        if (cnt > 0) {
                            running_dpu_mask |= (1ULL << each_dpu);
                            assigned_dpu |= (1ULL << each_dpu);
                            any_assigned = 1;
                        }
                        break;
                    }
                    // CAS failed: retry with fresh snapshot.
                }

                if(my_cnt > 0){
                    uint64_t base_idx = global_g->root_num[rank_id * 64 + each_dpu];
                    for(int i = 0; i < my_cnt; i++){
                        global_g->roots[rank_id * 64 + each_dpu][base_idx + i] = start_node + i;
                    }
                    global_g->root_num[rank_id * 64 + each_dpu] += my_cnt;
                }
                if(cnt > max_cnt) max_cnt = cnt;
                gettimeofday(&l_done, NULL);
                g_pcount_cs_hold_ms_by_thread[thread_id] += get_time_in_milliseconds(&l3, &l_done);


                off = global_offset[rank_id][each_dpu];
                global_offset[rank_id][each_dpu] += cnt;

                root_num_buf[each_dpu] = cnt;
                offset_buf[each_dpu] = off;
#ifdef OFIS_COLLECT_PER_BATCH
                // Only a real batch advances pb_last (the post-loop drain reads it).
                if (cnt > 0) {
                    pb_prev_off[each_dpu] = pb_last_off[each_dpu];
                    pb_prev_cnt[each_dpu] = pb_last_cnt[each_dpu];
                    pb_last_off[each_dpu] = off;
                    pb_last_cnt[each_dpu] = cnt;
                }
#endif
                if (cnt > 0) {
                    g_last_rootnum_by_thread[thread_id][each_dpu] = cnt;
                    g_last_offset_by_thread[thread_id][each_dpu] = off;
                    g_last_start_root_by_thread[thread_id][each_dpu] = start_node;
                    g_last_assign_iter_by_thread[thread_id][each_dpu] = loop_iter;
                }

                node_t *src_ptr = global_g->roots[rank_id * 64 + each_dpu] + off;
                roots_dst_buf[each_dpu] = src_ptr;
            }
        }
        gettimeofday(&assign_t1, NULL);
        g_assign_ms_by_thread[thread_id] += get_time_in_milliseconds(&assign_t0, &assign_t1);
        if (any_assigned) {
            if (g_use_batch_compact) {
                compact_payload_t *payloads = compact_payloads;
                uint64_t active_mask = 0;
                uint64_t max_root_count = 0;
                uint64_t max_row_bytes = 0;
                uint64_t max_col_bytes = 0;
                struct timeval cpt_t0, cpt_t1, xfer_t0, xfer_t1;
                gettimeofday(&cpt_t0, NULL);

                for (uint32_t i = 0; i < kDPUsPerRank; i++) {
                    payloads[i].valid = 0;
                    payloads[i].root_num = 0;
                    payloads[i].offset = 0;
                    payloads[i].row_bytes = 0;
                    payloads[i].col_bytes = 0;
                }

                DPU_FOREACH(rank, dpu, each_dpu) {
                    uint32_t ig_id = each_dpu % 8;
                    if ((assigned_dpu & (1ULL << each_dpu)) == 0) continue;
                    if (root_num_buf[each_dpu] == 0) continue;

                    uint64_t local_n = 0, local_m = 0;
                    compact_payload_t *pl = &payloads[each_dpu];
                    batch_compact_build(&compact_ctx, roots_dst_buf[each_dpu], root_num_buf[each_dpu], &local_n, &local_m);
                    pl->valid = 1;
                    pl->root_num = root_num_buf[each_dpu];
                    pl->offset = offset_buf[each_dpu];
                    pl->row_bytes = ALIGN8((local_n + 1) * sizeof(edge_ptr));
                    pl->col_bytes = ALIGN8(local_m * sizeof(node_t));
                    if (pl->root_num > max_root_count) max_root_count = pl->root_num;
                    compact_payload_ensure_root_capacity(pl, pl->root_num);
                    memcpy(pl->roots, compact_ctx.local_roots, (size_t)pl->root_num * sizeof(node_t));
                    memcpy(pl->row_ptr, compact_ctx.local_row_ptr, pl->row_bytes);
                    if (pl->col_bytes) {
                        memcpy(pl->col_idx, compact_ctx.local_col_idx, pl->col_bytes);
                    }
                    if (pl->row_bytes > max_row_bytes) max_row_bytes = pl->row_bytes;
                    if (pl->col_bytes > max_col_bytes) max_col_bytes = pl->col_bytes;
                    active_mask |= (1ULL << each_dpu);
                }
                gettimeofday(&cpt_t1, NULL);
                {
                    const double build_ms = get_time_in_milliseconds(&cpt_t0, &cpt_t1);
                    compact_build_ms += build_ms;
                    g_compact_build_ms_by_thread[thread_id] += build_ms;
                }

                if (active_mask != 0) {
                    const uint64_t active_ct = (uint64_t)popcount64(active_mask);
                    const uint64_t roots_bytes = ALIGN8(max_root_count * sizeof(node_t));
                    gettimeofday(&xfer_t0, NULL);
                    for (uint32_t i = 0; i < 64; i++) {
                        if (!payloads[i].valid) continue;
                        if (ALIGN8(payloads[i].root_num * sizeof(node_t)) < roots_bytes) {
                            compact_payload_ensure_root_capacity(&payloads[i], max_root_count);
                            memset((uint8_t *)payloads[i].roots + ALIGN8(payloads[i].root_num * sizeof(node_t)), 0,
                                   (size_t)(roots_bytes - ALIGN8(payloads[i].root_num * sizeof(node_t))));
                        }
                        if (payloads[i].row_bytes < max_row_bytes) {
                            memset((uint8_t *)payloads[i].row_ptr + payloads[i].row_bytes, 0,
                                   (size_t)(max_row_bytes - payloads[i].row_bytes));
                        }
                        if (payloads[i].col_bytes < max_col_bytes) {
                            memset((uint8_t *)payloads[i].col_idx + payloads[i].col_bytes, 0,
                                   (size_t)(max_col_bytes - payloads[i].col_bytes));
                        }
                    }

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
                        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].root_num);
                    }
                    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, &payloads[each_dpu].offset);
                    }
                    #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
                    DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
                    #endif

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].roots);
                    }
                    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0, roots_bytes);

                    DPU_FOREACH(rank, dpu, each_dpu) {
                        if (!(active_mask & (1ULL << each_dpu))) continue;
                        OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].row_ptr);
                    }
                    OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "row_ptr", 0, max_row_bytes);

                    /* Prepare only what is pushed: OFIS_push_xfer sends every prepared buffer. */
                    if (max_col_bytes > 0) {
                        DPU_FOREACH(rank, dpu, each_dpu) {
                            if (!(active_mask & (1ULL << each_dpu))) continue;
                            OFIS_prepare_xfer_dpu(dpu, active_mask, payloads[each_dpu].col_idx);
                        }
                        OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "col_idx", 0, max_col_bytes);
                    }
                    gettimeofday(&xfer_t1, NULL);
                    {
                        const double xfer_ms = get_time_in_milliseconds(&xfer_t0, &xfer_t1);
                        compact_xfer_ms += xfer_ms;
                        g_compact_xfer_ms_by_thread[thread_id] += xfer_ms;
                    }
                    g_b_roots_by_thread[thread_id] += active_ct * roots_bytes;
                    g_compact_batch_count_by_thread[thread_id]++;
                    g_compact_row_bytes_by_thread[thread_id] += active_ct * max_row_bytes;
                    g_compact_col_bytes_by_thread[thread_id] += active_ct * max_col_bytes;
                }
                goto dynamic_transfer_done;
            }
            const uint64_t finish_dpu = assigned_dpu;
            struct timeval t0, t1;
            const uint64_t finish_dpu_count = (uint64_t)popcount64(finish_dpu);
            const uint64_t roots_bytes = ALIGN8(max_cnt * sizeof(node_t));

            // send root num for each dpu
            gettimeofday(&t0, NULL);
            DPU_FOREACH(rank, dpu, each_dpu) {
                uint32_t ig_id = each_dpu % 8;
                OFIS_prepare_xfer_dpu(dpu, finish_dpu, &root_num_buf[each_dpu]);
            }
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "root_num", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
            gettimeofday(&t1, NULL);
            g_t_rootnum_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
            g_c_rootnum_by_thread[thread_id]++;
            g_n_rootnum_dpu_by_thread[thread_id] += finish_dpu_count;
            g_b_rootnum_by_thread[thread_id] += finish_dpu_count * sizeof(uint64_t);

            // send offset for each dpu
            gettimeofday(&t0, NULL);
            DPU_FOREACH(rank, dpu, each_dpu){
                uint32_t ig_id = each_dpu % 8;
                OFIS_prepare_xfer_dpu(dpu, finish_dpu, &offset_buf[each_dpu]);
            }
            #if !defined(OFIS_SCALAR_ANS) || defined(PERF)  /* scalar: no per-batch offset */
            DPU_ASSERT(dpu_push_xfer(rank, DPU_XFER_TO_DPU, "offset", 0, sizeof(uint64_t), DPU_XFER_DEFAULT));
            #endif

            gettimeofday(&t1, NULL);
            g_t_offset_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
            g_c_offset_by_thread[thread_id]++;
            g_n_offset_dpu_by_thread[thread_id] += finish_dpu_count;
            g_b_offset_by_thread[thread_id] += finish_dpu_count * sizeof(uint64_t);

            // send roots for each dpu
            gettimeofday(&t0, NULL);

            DPU_FOREACH(rank, dpu, each_dpu){
                uint32_t ig_id = each_dpu % 8;
                if(finish_dpu & (1ULL << each_dpu)){
                    OFIS_set_mux_dpu(rank, each_dpu, OPEN_TO_HOST);
                }
                OFIS_prepare_xfer_dpu(dpu, finish_dpu, roots_dst_buf[each_dpu]);
            }
            OFIS_push_xfer(rank, DPU_XFER_TO_DPU, "roots", 0,
                                     roots_bytes);

            gettimeofday(&t1, NULL);
            g_t_roots_ms_by_thread[thread_id] += get_time_in_milliseconds(&t0, &t1);
            g_c_roots_by_thread[thread_id]++;
            g_n_roots_dpu_by_thread[thread_id] += finish_dpu_count;
            g_b_roots_by_thread[thread_id] += finish_dpu_count * roots_bytes;
        }
dynamic_transfer_done:
        dpu_dbg_store(thread_id, DPU_DBG_STAGE_XFER, loop_iter, running_dpu_mask, finish_dpu, assigned_dpu);

        if (g_use_batch_compact) {
            struct timeval now_ts;
            gettimeofday(&now_ts, NULL);
            double since_last = get_time_in_milliseconds(&last_progress_ts, &now_ts);
            if (since_last >= 2000.0) {
                uint32_t running_ct = popcount64(running_dpu_mask);
                uint32_t finish_ct = popcount64(finish_dpu);
                uint32_t assigned_ct = popcount64(assigned_dpu);
                uint32_t p_snapshot = __atomic_load_n(&p_count, __ATOMIC_ACQUIRE);
                double elapsed = get_time_in_milliseconds(&loop_start_ts, &now_ts);
                last_progress_ts = now_ts;
                (void)p_snapshot;
            }
        }

        gettimeofday(&t_now, NULL);

        dpu_dbg_store(thread_id, DPU_DBG_STAGE_STATE_UPDATE, loop_iter, running_dpu_mask, finish_dpu, assigned_dpu);
        gettimeofday(&assign_t0, NULL);
        uint64_t state_update_dpu_count = 0;
        for(int i = 0; i < 64; ++i){
            uint32_t ig_id = i % 8;
            if(finish_dpu & (1ULL << i)){
                // Set MUX for DPUs
                OFIS_set_mux_dpu(rank, i, OPEN_TO_DPU);
                // Trigger DPU binary to continue iteration
                uint32_t next_state = (assigned_dpu & (1ULL << i)) ? 2 : 0;
                if (next_state == 0) {
                    const uint32_t pair_dpu = i ^ 1U;
                    const uint64_t pair_mask = (1ULL << pair_dpu);
                    const int pair_finished_now = ((finish_dpu & pair_mask) != 0);
                    const int pair_has_work     = ((assigned_dpu & pair_mask) != 0);
                    // Retire only when the PAIR is finished and idle: get_finished_dpu is pairwise, so a lone retired DPU is unreportable.
                    if (!pair_finished_now || pair_has_work) {
                        continue;
                    }
                }
                OFIS_set_state_dpu(rank, i, next_state);
                state_update_dpu_count++;
                dpu_last_timestamp[i] = t_now;
            }
        }
        gettimeofday(&assign_t1, NULL);
        g_state_update_ms_by_thread[thread_id] += get_time_in_milliseconds(&assign_t0, &assign_t1);
        g_c_state_update_calls_by_thread[thread_id]++;
        g_c_state_update_dpus_by_thread[thread_id] += state_update_dpu_count;
        if(running_dpu_mask == 0) {
#ifdef OFIS_COLLECT_ON_FINAL_RETIRE
            ofis_collect_results_for_mask(thread_id, rank_id, rank, my_dpu_mask);
#endif
            finish = 1;
        }
    }
#if defined(OFIS_COLLECT_PER_BATCH) && !defined(OFIS_COLLECT_NO_LAG)
    // Tail drain: all DPUs retired, read each DPU's one-round-lagged last batch.
    ofis_collect_batch_slice(thread_id, rank_id, rank,
                             my_dpu_mask, pb_last_off, pb_last_cnt);
#endif
    dpu_dbg_store(thread_id, DPU_DBG_STAGE_EXIT, loop_iter, 0, finish_dpu, 0);
    __atomic_store_n(&g_dpu_dbg[thread_id].done, 1, __ATOMIC_RELAXED);
    // staging free
    free(root_num_buf);
    free(offset_buf);
    free(roots_dst_buf);
    for (uint32_t i = 0; i < kDPUsPerRank; ++i) {
        if (initial_roots_payloads[i]) {
            free(initial_roots_payloads[i]);
        }
    }
    free(initial_roots_payloads);
    free(batch_bitmap);
    free(batch_touched_marks);
    free(batch_touched_words);
    if (g_use_batch_compact) {
        for (uint32_t i = 0; i < kDPUsPerRank; i++) {
            compact_payload_reset(&compact_payloads[i]);
        }
        free(compact_payloads);
        batch_compact_ctx_free(&compact_ctx);
    }

    return NULL;
}

#ifdef OFIS_HYBRID
void* thread_hybrid_fct(void* arg){
    thread_args* a = (thread_args*)arg;
    uint32_t thread_id = a->thread_id;
    uint32_t rank_id   = thread_id;                 /* one worker thread per rank */
    struct dpu_set_t rank = a->rank;

    cpu_set_t cpu_set; CPU_ZERO(&cpu_set); CPU_SET(thread_id, &cpu_set);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);

    struct dpu_set_t dpu; uint32_t each_dpu;
    const uint32_t kDPUsPerRank = 64;
    const uint32_t kDPUsPerIG   = 8;

    uint64_t *root_num_buf  = (uint64_t*)malloc(sizeof(uint64_t) * kDPUsPerRank);
    uint64_t *offset_buf    = (uint64_t*)malloc(sizeof(uint64_t) * kDPUsPerRank);
    node_t  **roots_dst_buf = (node_t**)malloc(sizeof(node_t*) * kDPUsPerRank);

    batch_compact_ctx_t compact_ctx;
    compact_payload_t *compact_payloads = NULL;
    uint32_t *batch_bitmap = NULL; uint8_t *batch_touched_marks = NULL;
    uint32_t *batch_touched_words = NULL; uint32_t batch_touched_count = 0;
    {
        const uint32_t n_words = (global_g->n + 31U) >> 5;
        batch_compact_ctx_init(&compact_ctx, global_g->n);
        batch_bitmap        = calloc((size_t)n_words, sizeof(uint32_t));
        batch_touched_marks = calloc((size_t)n_words, sizeof(uint8_t));
        batch_touched_words = malloc((size_t)n_words * sizeof(uint32_t));
        compact_payloads    = calloc(kDPUsPerRank, sizeof(compact_payload_t));
        if (!root_num_buf || !offset_buf || !roots_dst_buf || !batch_bitmap ||
            !batch_touched_marks || !batch_touched_words || !compact_payloads) {
            printf(ANSI_COLOR_RED "Error: hybrid scratch alloc failed\n" ANSI_COLOR_RESET);
            exit(1);
        }
        compact_payloads_init(compact_payloads, kDPUsPerRank);
    }

    uint64_t running_dpu_mask = 0;   /* phase 1: per-DPU heavy */
    uint8_t  running_ig_mask  = 0;   /* phase 2: per-IG light  */

#ifdef OFIS_COLLECT_PER_BATCH
    uint64_t pb_prev_off[64]={0}, pb_prev_cnt[64]={0};
    uint64_t pb_last_off[64]={0}, pb_last_cnt[64]={0};
#endif

    // ======================= PHASE 1: heavy, DPU granularity ================
    struct timeval ph_w0, ph_w1, ph_s0, ph_s1;
#ifdef DPU_TIME_PROFILE
    uint64_t ig_run_mask = 0, rank_run_mask = 0;   /* DPUs actually launched in IG/RANK tier */
#endif
    double ph_x0, ph_b0, ph_c0;
    #define PH_STAGE_SNAP()  do { ph_x0 = g_compact_xfer_ms_by_thread[thread_id]; \
        ph_b0 = g_compact_build_ms_by_thread[thread_id]; \
        ph_c0 = g_answer_collect_ms_by_thread[thread_id]; } while(0)
    #define PH_STAGE_ACC(P)  do { \
        g_hybrid_phase_xfer_ms[P][thread_id]  += g_compact_xfer_ms_by_thread[thread_id]  - ph_x0; \
        g_hybrid_phase_build_ms[P][thread_id] += g_compact_build_ms_by_thread[thread_id] - ph_b0; \
        g_hybrid_phase_coll_ms[P][thread_id]  += g_answer_collect_ms_by_thread[thread_id] - ph_c0; } while(0)
    #define PH_STRAG_ACC(P, first_tv, done_tv)  do { \
        double _sp = get_time_in_milliseconds(&(first_tv), &(done_tv)); \
        if (_sp < 0.0) _sp = 0.0; \
        g_hybrid_phase_strag_sum_ms[P][thread_id] += _sp; \
        g_hybrid_phase_strag_cnt[P][thread_id]++; \
        if (_sp > g_hybrid_phase_strag_max_ms[P][thread_id]) g_hybrid_phase_strag_max_ms[P][thread_id] = _sp; } while(0)

    // A RANK phase follows iff the RANK region is non-empty.
#if defined(BATCH_TIER_BY_COUNT) && defined(OFIS_PREBUILD_ALL)
    const int have_phase3 = (g_split2_batch_idx < g_prebuilt_count);   /* prebuild: batch-index cut */
#else
    const int have_phase3 = (g_split2 < (node_t)g_total_roots);        /* pipeline: g_split2 root boundary */
#endif

#if defined(OFIS_HYBRID_EARLY_PROMOTE) && defined(OFIS_COLLECT_PER_BATCH)
#error "OFIS_HYBRID_EARLY_PROMOTE currently supports final-retire collect only (not OFIS_COLLECT_PER_BATCH)"
#endif

#ifndef OFIS_HYBRID_EARLY_PROMOTE
    g_hybrid_pull_tier = 0;
    gettimeofday(&ph_w0, NULL);
    PH_STAGE_SNAP();
    {
        uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ~0ULL,
            &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
            batch_touched_words, &batch_touched_count,
            root_num_buf, offset_buf, roots_dst_buf);
        DPU_FOREACH(rank, dpu, each_dpu){
            // Track EVERY launched DPU so the drain waits for all to quiesce before phase 2 flips mux/state.
            running_dpu_mask |= (1ULL << each_dpu);
#ifdef OFIS_COLLECT_PER_BATCH
            if (active & (1ULL << each_dpu)) {
                pb_last_off[each_dpu] = offset_buf[each_dpu];
                pb_last_cnt[each_dpu] = root_num_buf[each_dpu];
            }
#endif
        }
        (void)active;
    }
    OFIS_STARTUP_SYNC();               /* no-op unless OFIS_STARTUP_BARRIER */
    OFIS_dpu_launch(rank);
    OFIS_STAMP_FIRST_LAUNCH(thread_id);
#ifdef DPU_TIME_PROFILE
    { struct timeval now; gettimeofday(&now, NULL);
      for (uint32_t i = 0; i < 64; ++i) if (running_dpu_mask & (1ULL << i)) {
          g_dpu_launch_tv[rank_id*64+i] = now; g_dpu_launch_cnt[rank_id*64+i]++; } }
#endif

    int finish = 0;
    uint64_t finish_dpu = 0;
    while (!finish){
        if (running_dpu_mask == 0) break;   /* every heavy DPU parked -> drained */
        uint32_t nr = 0;
        gettimeofday(&ph_s0, NULL);
        do {
            nr = OFIS_get_finished_dpu(rank, &finish_dpu);
            finish_dpu &= running_dpu_mask;
            if (finish_dpu == 0) nr = 0;
            if (nr == 0) POLL_BACKOFF();
        } while (nr == 0);
        gettimeofday(&ph_s1, NULL);
        {
            double rw = get_time_in_milliseconds(&ph_s0, &ph_s1);
            g_hybrid_phase_wait_ms[0][thread_id] += rw;
            int busy = __builtin_popcountll(running_dpu_mask);
            if (busy < 64){
                g_hybrid_phase_tail_ms[0][thread_id]     += rw;
                g_hybrid_phase_idle_dpu_ms[0][thread_id] += rw * (double)(64 - busy);
            }
        }
#ifdef DPU_TIME_PROFILE
        // per-DPU kernel wall for this batch = finish(ph_s1) - its last launch
        for (uint32_t i = 0; i < 64; ++i) if (finish_dpu & (1ULL << i)) {
            const uint32_t gd = rank_id*64+i;
            const double _k = get_time_in_milliseconds(&g_dpu_launch_tv[gd], &ph_s1);
            g_dpu_kernel_ms[gd] += _k;
            g_dpu_finish_tv[gd] = ph_s1;    /* mark finish; idle accrues until relaunch */
        }
#endif

        for (uint32_t i = 0; i < 64; ++i)
            if (finish_dpu & (1ULL << i)) OFIS_set_mux_dpu(rank, i, OPEN_TO_HOST);

#ifdef OFIS_COLLECT_PER_BATCH
        collect_slice_nomux(thread_id, rank_id, rank, finish_dpu & running_dpu_mask,
                            pb_prev_off, pb_prev_cnt);
#endif

        // Refill HEAVY for the finished DPUs; any that get nothing PARK.
        uint64_t refill = compact_dispatch_masked(thread_id, rank_id, rank,
            finish_dpu & running_dpu_mask,
            &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
            batch_touched_words, &batch_touched_count,
            root_num_buf, offset_buf, roots_dst_buf);

        uint64_t parked_now = 0;
        for (uint32_t i = 0; i < 64; ++i){
            if (!(finish_dpu & (1ULL << i))) continue;
            if (refill & (1ULL << i)){
#ifdef OFIS_COLLECT_PER_BATCH
                pb_prev_off[i] = pb_last_off[i]; pb_prev_cnt[i] = pb_last_cnt[i];
                pb_last_off[i] = offset_buf[i];  pb_last_cnt[i] = root_num_buf[i];
#endif
                OFIS_set_mux_dpu(rank, i, OPEN_TO_DPU);
                OFIS_set_state_dpu(rank, i, 2);
#ifdef DPU_TIME_PROFILE
                { const uint32_t gd = rank_id*64+i; struct timeval now; gettimeofday(&now, NULL);
                  g_dpu_idle_ms[gd] += get_time_in_milliseconds(&g_dpu_finish_tv[gd], &now);
                  g_dpu_launch_tv[gd] = now; g_dpu_launch_cnt[gd]++; }
#endif
            } else {
                // PARK: no heavy work left; leave state 1 + mux HOST for phase 2.
                parked_now |= (1ULL << i);
                running_dpu_mask &= ~(1ULL << i);
            }
        }
#ifdef OFIS_COLLECT_PER_BATCH
        // Flush parked DPUs' last heavy batch (collect handles it itself).
        if (parked_now){
            collect_slice_nomux(thread_id, rank_id, rank, parked_now,
                                pb_last_off, pb_last_cnt);
            for (uint32_t i = 0; i < 64; ++i)
                if (parked_now & (1ULL << i)){ pb_prev_cnt[i] = 0; pb_last_cnt[i] = 0; }
        }
#endif
    }
    gettimeofday(&ph_w1, NULL);
    g_hybrid_phase_wall_ms[0][thread_id] += get_time_in_milliseconds(&ph_w0, &ph_w1);
    PH_STAGE_ACC(0);

    // ======================= PHASE 2: medium, IG granularity ===============
    // If phase 3 follows, phase 2 leaves its DPUs PARKED (state 1), not terminated.
    g_hybrid_pull_tier = 1;
    running_ig_mask = 0;
    gettimeofday(&ph_w0, NULL);
    PH_STAGE_SNAP();
    {
        uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ~0ULL,
            &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
            batch_touched_words, &batch_touched_count,
            root_num_buf, offset_buf, roots_dst_buf);
        DPU_FOREACH(rank, dpu, each_dpu){
            if (!(active & (1ULL << each_dpu))) continue;
            running_ig_mask |= (uint8_t)(1u << (each_dpu % kDPUsPerIG));
#ifdef OFIS_COLLECT_PER_BATCH
            pb_last_off[each_dpu] = offset_buf[each_dpu];
            pb_last_cnt[each_dpu] = root_num_buf[each_dpu];
#endif
        }
        for (uint32_t ig = 0; ig < 8; ++ig){
            if (!((running_ig_mask >> ig) & 1)) continue;
            OFIS_set_mux_ig(rank, ig, OPEN_TO_DPU);
        }
        // Wake ONLY the DPUs that got a light batch: an un-fed DPU woken by set_state_ig would re-run its stale heavy batch.
        DPU_FOREACH(rank, dpu, each_dpu){
            if (active & (1ULL << each_dpu)) OFIS_set_state_dpu(rank, each_dpu, 2);
        }
#ifdef DPU_TIME_PROFILE
        { struct timeval now; gettimeofday(&now, NULL);
          for (uint32_t i = 0; i < 64; ++i) if (active & (1ULL << i)) {
              g_dpu_launch_tv[rank_id*64+i] = now; g_dpu_launch_cnt[rank_id*64+i]++; }
          ig_run_mask = active; }
#endif
    }
    // DPU->IG barrier gap: drain-complete (ph_w1) to first IG wake, all DPUs idle.
    { struct timeval t_p1w; gettimeofday(&t_p1w, NULL);
      g_hybrid_trans_gap_ms[1][thread_id] += get_time_in_milliseconds(&ph_w1, &t_p1w); }
    const int ig_full = __builtin_popcount(running_ig_mask);   /* IGs fed at phase entry */

    finish = 0;
    uint8_t finish_ig = 0;
    while (!finish){
        if (running_ig_mask == 0) break;
        uint32_t nr_finish_ig = 0;
        uint64_t ig_amask = 0;
        for (uint32_t d = 0; d < 64; ++d)
            if ((running_ig_mask >> (d % kDPUsPerIG)) & 1) ig_amask |= (1ULL << d);
        struct timeval ph_f; int have_f = 0; uint64_t fdpu = 0;
        gettimeofday(&ph_s0, NULL);
        do {
            nr_finish_ig = OFIS_get_finished_ig(rank, &finish_ig);
            if (!have_f){ OFIS_get_finished_dpu(rank, &fdpu);
                          if (fdpu & ig_amask){ gettimeofday(&ph_f, NULL); have_f = 1; } }
            if (nr_finish_ig == 0) POLL_BACKOFF();
        } while (nr_finish_ig == 0);
        gettimeofday(&ph_s1, NULL);
        {
            double rw = get_time_in_milliseconds(&ph_s0, &ph_s1);
            g_hybrid_phase_wait_ms[1][thread_id] += rw;
            int busy_ig = __builtin_popcount(running_ig_mask);
            if (busy_ig < ig_full){
                g_hybrid_phase_tail_ms[1][thread_id]     += rw;
                g_hybrid_phase_idle_dpu_ms[1][thread_id] += rw *
                    (double)((ig_full - busy_ig) * (int)kDPUsPerIG);
            }
        }
        if (have_f) PH_STRAG_ACC(1, ph_f, ph_s1);

        for (uint32_t i = 0; i < 8; ++i)
            if (finish_ig & (1 << i)) OFIS_set_mux_ig(rank, i, OPEN_TO_HOST);

#ifdef OFIS_COLLECT_PER_BATCH
        {
            uint64_t ig_collect_mask = 0;
            for (uint32_t d = 0; d < 64; ++d)
                if ((finish_ig >> (d % kDPUsPerIG)) & 1) ig_collect_mask |= (1ULL << d);
            collect_slice_nomux(thread_id, rank_id, rank, ig_collect_mask,
                                pb_prev_off, pb_prev_cnt);
        }
#endif

        uint64_t fin_mask = 0;
        for (uint32_t d = 0; d < 64; ++d){
            uint32_t ig = d % kDPUsPerIG;
            if (((finish_ig >> ig) & 1) && ((running_ig_mask >> ig) & 1))
                fin_mask |= (1ULL << d);
        }
        for (uint32_t ig = 0; ig < 8; ++ig)
            if ((finish_ig >> ig) & 1) running_ig_mask &= ~(uint8_t)(1u << ig);
#ifdef DPU_TIME_PROFILE
        for (uint32_t i = 0; i < 64; ++i) if ((fin_mask & ig_run_mask) & (1ULL << i)) {
            const uint32_t gd = rank_id*64+i;
            const double _k = get_time_in_milliseconds(&g_dpu_launch_tv[gd], &ph_s1);
            g_dpu_kernel_ms[gd] += _k;
        }
        ig_run_mask &= ~fin_mask;
#endif

        uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, fin_mask,
            &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
            batch_touched_words, &batch_touched_count,
            root_num_buf, offset_buf, roots_dst_buf);

        DPU_FOREACH(rank, dpu, each_dpu){
            if (!(active & (1ULL << each_dpu))) continue;
            running_ig_mask |= (uint8_t)(1u << (each_dpu % kDPUsPerIG));
#ifdef OFIS_COLLECT_PER_BATCH
            if (root_num_buf[each_dpu] > 0){
                pb_prev_off[each_dpu] = pb_last_off[each_dpu];
                pb_prev_cnt[each_dpu] = pb_last_cnt[each_dpu];
                pb_last_off[each_dpu] = offset_buf[each_dpu];
                pb_last_cnt[each_dpu] = root_num_buf[each_dpu];
            }
#endif
        }
        // Finished-IG set state: 2=run, 1=park (phase 3 follows), 0=retire; keeps IG pairs synced for get_finished_ig.
        const uint32_t cstate = (active != 0) ? 2 : (have_phase3 ? 1 : 0);
        for (uint32_t ig = 0; ig < 8; ++ig){
            if (!((finish_ig >> ig) & 1)) continue;
            OFIS_set_mux_ig(rank, ig, OPEN_TO_DPU);
            OFIS_set_state_ig(rank, ig, cstate);
        }
#ifdef DPU_TIME_PROFILE
        if (active) { struct timeval now; gettimeofday(&now, NULL);
          for (uint32_t i = 0; i < 64; ++i) if (active & (1ULL << i)) {
              g_dpu_launch_tv[rank_id*64+i] = now; g_dpu_launch_cnt[rank_id*64+i]++; }
          ig_run_mask |= active; }
#endif
        if (running_ig_mask == 0) finish = 1;
    }
    gettimeofday(&ph_w1, NULL);
    g_hybrid_phase_wall_ms[1][thread_id] += get_time_in_milliseconds(&ph_w0, &ph_w1);
    PH_STAGE_ACC(1);
#else  /* OFIS_HYBRID_EARLY_PROMOTE: merged Phase 1+2 with per-IG-pair early transition */
    // Promote each IG-pair from heavy(DPU) to light(IG) when its heavy DPUs park, hiding the phase-1 drain tail behind other pairs' light work.
    enum { EP_HEAVY, EP_LIGHT, EP_DRAINED };
    int      pair_mode[4] = { EP_HEAVY, EP_HEAVY, EP_HEAVY, EP_HEAVY };
    uint8_t  light_run_ig = 0;  /* IGs of LIGHT pairs still running */
    static const uint64_t ep_pairmask[4] = {
        0x0303030303030303ULL, 0x0C0C0C0C0C0C0C0CULL,
        0x3030303030303030ULL, 0xC0C0C0C0C0C0C0C0ULL };

    g_hybrid_pull_tier = 0;
    gettimeofday(&ph_w0, NULL);
    PH_STAGE_SNAP();
    (void)compact_dispatch_masked(thread_id, rank_id, rank, ~0ULL,
        &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
        batch_touched_words, &batch_touched_count,
        root_num_buf, offset_buf, roots_dst_buf);
    for (uint32_t d = 0; d < 64; ++d) running_dpu_mask |= (1ULL << d);
    OFIS_STARTUP_SYNC();
    OFIS_dpu_launch(rank);
    OFIS_STAMP_FIRST_LAUNCH(thread_id);

    for (;;) {
        int active_pairs = 0;
        for (int p = 0; p < 4; ++p) if (pair_mode[p] != EP_DRAINED) { active_pairs = 1; break; }
        if (!active_pairs) break;
        int progress = 0;

        // (A) heavy DPUs of HEAVY pairs: refill heavy, or park.
        uint64_t heavy_dmask = 0;
        for (int p = 0; p < 4; ++p)
            if (pair_mode[p] == EP_HEAVY) heavy_dmask |= ep_pairmask[p];
        if (heavy_dmask) {
            uint64_t fdpu = 0;
            OFIS_get_finished_dpu(rank, &fdpu);
            fdpu &= heavy_dmask & running_dpu_mask;
            if (fdpu) {
                progress = 1;
                for (uint32_t d = 0; d < 64; ++d)
                    if (fdpu & (1ULL << d)) OFIS_set_mux_dpu(rank, d, OPEN_TO_HOST);
                g_hybrid_pull_tier = 0;
                uint64_t refill = compact_dispatch_masked(thread_id, rank_id, rank, fdpu,
                    &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
                    batch_touched_words, &batch_touched_count,
                    root_num_buf, offset_buf, roots_dst_buf);
                for (uint32_t d = 0; d < 64; ++d) {
                    if (!(fdpu & (1ULL << d))) continue;
                    if (refill & (1ULL << d)) {
                        OFIS_set_mux_dpu(rank, d, OPEN_TO_DPU);
                        OFIS_set_state_dpu(rank, d, 2);
                    } else {                       /* park: keep state 1, mux HOST */
                        running_dpu_mask &= ~(1ULL << d);
                    }
                }
            }
        }

        // (B) promote pairs whose heavy DPUs all parked -> LIGHT (or drained); wake only the fed DPUs.
        for (int p = 0; p < 4; ++p) {
            if (pair_mode[p] != EP_HEAVY) continue;
            if (running_dpu_mask & ep_pairmask[p]) continue;   /* heavy not fully parked yet */
            progress = 1;
            g_hybrid_pull_tier = 1;
            uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ep_pairmask[p],
                &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
                batch_touched_words, &batch_touched_count,
                root_num_buf, offset_buf, roots_dst_buf);
            OFIS_set_mux_ig(rank, 2 * p,     OPEN_TO_DPU);
            OFIS_set_mux_ig(rank, 2 * p + 1, OPEN_TO_DPU);
            if (active) {
                for (uint32_t d = 0; d < 64; ++d)                     /* fed only: un-fed hold stale heavy */
                    if (active & (1ULL << d)) OFIS_set_state_dpu(rank, d, 2);
            } else {
                OFIS_set_state_ig(rank, 2 * p,     have_phase3 ? 1 : 0);   /* no light -> park/retire pair */
                OFIS_set_state_ig(rank, 2 * p + 1, have_phase3 ? 1 : 0);
            }
            if (active) {
                pair_mode[p] = EP_LIGHT;
                light_run_ig |= (uint8_t)((1u << (2 * p)) | (1u << (2 * p + 1)));
            } else {
                pair_mode[p] = EP_DRAINED;
            }
        }

        // (C) LIGHT pairs whose BOTH IGs finished -> refill light, or drain.
        if (light_run_ig) {
            uint8_t fig = 0;
            OFIS_get_finished_ig(rank, &fig);
            fig &= light_run_ig;
            for (int p = 0; p < 4; ++p) {
                if (pair_mode[p] != EP_LIGHT) continue;
                uint8_t pbits = (uint8_t)((1u << (2 * p)) | (1u << (2 * p + 1)));
                if ((fig & pbits) != pbits) continue;   /* both IGs of the pair must be done */
                progress = 1;
                OFIS_set_mux_ig(rank, 2 * p,     OPEN_TO_HOST);
                OFIS_set_mux_ig(rank, 2 * p + 1, OPEN_TO_HOST);
                g_hybrid_pull_tier = 1;
                uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ep_pairmask[p],
                    &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
                    batch_touched_words, &batch_touched_count,
                    root_num_buf, offset_buf, roots_dst_buf);
                OFIS_set_mux_ig(rank, 2 * p,     OPEN_TO_DPU);
                OFIS_set_mux_ig(rank, 2 * p + 1, OPEN_TO_DPU);
                {
                    uint32_t cstate = (active != 0) ? 2 : (have_phase3 ? 1 : 0);
                    OFIS_set_state_ig(rank, 2 * p,     cstate);
                    OFIS_set_state_ig(rank, 2 * p + 1, cstate);
                }
                if (!active) { light_run_ig &= ~pbits; pair_mode[p] = EP_DRAINED; }
            }
        }

        if (!progress) {
            POLL_BACKOFF();
        }
    }
    gettimeofday(&ph_w1, NULL);
    g_hybrid_phase_wall_ms[0][thread_id] += get_time_in_milliseconds(&ph_w0, &ph_w1);
    PH_STAGE_ACC(0);
#endif /* OFIS_HYBRID_EARLY_PROMOTE */

    // ======================= PHASE 3: light, RANK granularity ===============
    if (have_phase3){
#if defined(OFIS_COLLECT_PER_BATCH) && !defined(OFIS_COLLECT_NO_LAG)
        // Flush phase-2's last lagged batch before phase 3 reuses pb_last.
        {
            uint64_t tail = 0; for (uint32_t d = 0; d < 64; ++d) tail |= (1ULL << d);
            OFIS_set_mux_rank(rank, OPEN_TO_HOST);
            collect_slice_nomux(thread_id, rank_id, rank, tail, pb_last_off, pb_last_cnt);
            for (uint32_t i = 0; i < 64; ++i){
                pb_prev_off[i] = pb_prev_cnt[i] = 0;
                pb_last_off[i] = pb_last_cnt[i] = 0;
            }
        }
#endif
        g_hybrid_pull_tier = 2;
        gettimeofday(&ph_w0, NULL);
        PH_STAGE_SNAP();
        int running_rank = 0;
        {
            OFIS_set_mux_rank(rank, OPEN_TO_HOST);
            uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ~0ULL,
                &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
                batch_touched_words, &batch_touched_count,
                root_num_buf, offset_buf, roots_dst_buf);
            OFIS_set_mux_rank(rank, OPEN_TO_DPU);
            DPU_FOREACH(rank, dpu, each_dpu){
                if (active & (1ULL << each_dpu)){
                    OFIS_set_state_dpu(rank, each_dpu, 2);
#ifdef OFIS_COLLECT_PER_BATCH
                    pb_last_off[each_dpu] = offset_buf[each_dpu];
                    pb_last_cnt[each_dpu] = root_num_buf[each_dpu];
#endif
                }
            }
#ifdef DPU_TIME_PROFILE
            { struct timeval now; gettimeofday(&now, NULL);
              for (uint32_t i = 0; i < 64; ++i) if (active & (1ULL << i)) {
                  g_dpu_launch_tv[rank_id*64+i] = now; g_dpu_launch_cnt[rank_id*64+i]++; }
              rank_run_mask = active; }
#endif
            running_rank = (active != 0);
        }
        // IG->RANK barrier gap: phase-2 drain-complete (ph_w1) to first RANK wake.
        { struct timeval t_p2w; gettimeofday(&t_p2w, NULL);
          g_hybrid_trans_gap_ms[2][thread_id] += get_time_in_milliseconds(&ph_w1, &t_p2w); }
        while (running_rank){
            uint32_t rank_done = 0;
            struct timeval ph_f; int have_f = 0; uint64_t fdpu = 0;
            gettimeofday(&ph_s0, NULL);
            do {
                rank_done = OFIS_get_finished_rank(rank);
                if (!have_f){ OFIS_get_finished_dpu(rank, &fdpu);
                              if (fdpu){ gettimeofday(&ph_f, NULL); have_f = 1; } }
                if (!rank_done) POLL_BACKOFF();
            } while (!rank_done);
            gettimeofday(&ph_s1, NULL);
            g_hybrid_phase_wait_ms[2][thread_id] += get_time_in_milliseconds(&ph_s0, &ph_s1);
#ifdef DPU_TIME_PROFILE
            for (uint32_t i = 0; i < 64; ++i) if (rank_run_mask & (1ULL << i)) {
                const uint32_t gd = rank_id*64+i;
                const double _k = get_time_in_milliseconds(&g_dpu_launch_tv[gd], &ph_s1);
                g_dpu_kernel_ms[gd] += _k;
            }
            rank_run_mask = 0;
#endif
            if (have_f) PH_STRAG_ACC(2, ph_f, ph_s1);

            OFIS_set_mux_rank(rank, OPEN_TO_HOST);
#ifdef OFIS_COLLECT_PER_BATCH
            collect_slice_nomux(thread_id, rank_id, rank, ~0ULL, pb_prev_off, pb_prev_cnt);
#endif
            uint64_t active = compact_dispatch_masked(thread_id, rank_id, rank, ~0ULL,
                &compact_ctx, compact_payloads, batch_bitmap, batch_touched_marks,
                batch_touched_words, &batch_touched_count,
                root_num_buf, offset_buf, roots_dst_buf);
#ifdef OFIS_COLLECT_PER_BATCH
            DPU_FOREACH(rank, dpu, each_dpu){
                if ((active & (1ULL << each_dpu)) && root_num_buf[each_dpu] > 0){
                    pb_prev_off[each_dpu] = pb_last_off[each_dpu];
                    pb_prev_cnt[each_dpu] = pb_last_cnt[each_dpu];
                    pb_last_off[each_dpu] = offset_buf[each_dpu];
                    pb_last_cnt[each_dpu] = root_num_buf[each_dpu];
                }
            }
#endif
            if (active != 0){
                OFIS_set_mux_rank(rank, OPEN_TO_DPU);
                DPU_FOREACH(rank, dpu, each_dpu){
                    if (active & (1ULL << each_dpu)) OFIS_set_state_dpu(rank, each_dpu, 2);
                }
#ifdef DPU_TIME_PROFILE
                { struct timeval now; gettimeofday(&now, NULL);
                  for (uint32_t i = 0; i < 64; ++i) if (active & (1ULL << i)) {
                      g_dpu_launch_tv[rank_id*64+i] = now; g_dpu_launch_cnt[rank_id*64+i]++; }
                  rank_run_mask = active; }
#endif
            } else {
                OFIS_set_mux_rank(rank, OPEN_TO_DPU);
                OFIS_set_state_rank(rank, 0);
                running_rank = 0;
            }
        }
        gettimeofday(&ph_w1, NULL);
        g_hybrid_phase_wall_ms[2][thread_id] += get_time_in_milliseconds(&ph_w0, &ph_w1);
        PH_STAGE_ACC(2);
    }
    #undef PH_STAGE_SNAP
    #undef PH_STAGE_ACC
    #undef PH_STRAG_ACC

#if defined(OFIS_COLLECT_PER_BATCH) && !defined(OFIS_COLLECT_NO_LAG)
    {
        uint64_t tail_mask = 0;
        for (uint32_t ig = 0; ig < 8; ++ig) OFIS_set_mux_ig(rank, ig, OPEN_TO_HOST);
        for (uint32_t d = 0; d < 64; ++d) tail_mask |= (1ULL << d);
        collect_slice_nomux(thread_id, rank_id, rank, tail_mask, pb_last_off, pb_last_cnt);
    }
#endif
#ifndef OFIS_COLLECT_PER_BATCH
    // AT_END / ON_FINAL_RETIRE: one settled sweep of the whole accumulated ans.
    {
        uint64_t collect_mask = 0;
        for (uint32_t ig = 0; ig < 8; ++ig) OFIS_set_mux_ig(rank, ig, OPEN_TO_HOST);
        for (uint32_t d = 0; d < 64; ++d) collect_mask |= (1ULL << d);
        collect_full_direct_nomux(thread_id, rank_id, rank, collect_mask);
    }
#endif

    free(root_num_buf);
    free(offset_buf);
    free(roots_dst_buf);
    free(batch_bitmap);
    free(batch_touched_marks);
    free(batch_touched_words);
    for (uint32_t i = 0; i < kDPUsPerRank; i++) compact_payload_reset(&compact_payloads[i]);
    free(compact_payloads);
    batch_compact_ctx_free(&compact_ctx);
    return NULL;
}
#endif /* OFIS_HYBRID */

void OFIS_data_transfer(struct dpu_set_t set, Graph *g) {
    global_g = g;
    g_use_batch_compact = 0;
    struct timeval start_time, end_time;
    FILE *fp_time_tf;
    fp_time_tf = fopen("./result/" "result_"PATTERN_NAME "_" DATA_NAME ".txt", "w");

    // Measure time for read_input
    gettimeofday(&start_time, NULL);
    read_input();
    gettimeofday(&end_time, NULL);
    printf("Data reading Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    struct dpu_set_t dpu;
    uint32_t each_dpu;
    
    fprintf(fp_time_tf, "Data reading Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    
    gettimeofday(&start_time, NULL);
    data_renumber();
    gettimeofday(&end_time, NULL);
    printf("Data Sorting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_tf, "Data Sorting Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    g_use_batch_compact = (global_g->n > DPU_N - 1 || global_g->m > DPU_M);

    gettimeofday(&start_time, NULL);
    // Pin the refill batch to a constant.
    g_auto_batch_size = (OFIS_FIXED_BATCH < 1) ? 1
                      : (OFIS_FIXED_BATCH > DPU_ROOT_NUM) ? DPU_ROOT_NUM
                      : (uint32_t)(OFIS_FIXED_BATCH);
    printf("Optimal k: (fixed batch override)\n");
    printf("Fixed batch size: %u\n", g_auto_batch_size);
    gettimeofday(&end_time, NULL);
    printf("Optimal Allocating Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));
    fprintf(fp_time_tf, "Optimal Allocating Time (ms): %.6f\n", get_time_in_milliseconds(&start_time, &end_time));

    gettimeofday(&start_time, NULL);
    // Initialize transferring data buffer
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        global_g->root_num[i] = 0;
        if (!g_root_buf_storage[i]) {
            g_root_buf_storage[i] = malloc((DPU_ROOT_NUM + 64) * sizeof(node_t));
            assert(g_root_buf_storage[i]);
        }
        global_g->roots[i] = g_root_buf_storage[i];
    }

    // 1. Initialize root distribution state
    g_total_roots = global_g->n;
    init_off_alloc();

    // 2. Broadcast CSR
#if !defined(OFIS_THREAD_INIT_CSR_XFER)
    if (!g_use_batch_compact) {
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->row_ptr));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "row_ptr", 0, ALIGN8((global_g->n + 1) * sizeof(edge_ptr)), DPU_XFER_DEFAULT));
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, global_g->col_idx));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_TO_DPU, "col_idx", 0, ALIGN8(global_g->m * sizeof(node_t)), DPU_XFER_DEFAULT));
    }
    gettimeofday(&end_time, NULL);
    printf("OFIS initial transfer Time (ms): %f\n", get_time_in_milliseconds(&start_time, &end_time));
#else
    gettimeofday(&end_time, NULL);
    printf("OFIS initial setup Time (ms): %f\n", get_time_in_milliseconds(&start_time, &end_time));
#endif
        
    gettimeofday(&start_time, NULL);
    p_count = 0;
    memset(g_wait_ms_by_thread, 0, sizeof(g_wait_ms_by_thread));
    memset(g_barrier_wait_ms_by_thread, 0, sizeof(g_barrier_wait_ms_by_thread));
    memset(g_assign_ms_by_thread, 0, sizeof(g_assign_ms_by_thread));
    memset(g_mux_host_ms_by_thread, 0, sizeof(g_mux_host_ms_by_thread));
    memset(g_state_update_ms_by_thread, 0, sizeof(g_state_update_ms_by_thread));
    memset(g_pcount_lock_wait_ms_by_thread, 0, sizeof(g_pcount_lock_wait_ms_by_thread));
    memset(g_pcount_cs_hold_ms_by_thread, 0, sizeof(g_pcount_cs_hold_ms_by_thread));
    memset(g_pcount_lock_acqs_by_thread, 0, sizeof(g_pcount_lock_acqs_by_thread));
    memset(g_t_rootnum_ms_by_thread, 0, sizeof(g_t_rootnum_ms_by_thread));
    memset(g_t_offset_ms_by_thread, 0, sizeof(g_t_offset_ms_by_thread));
    memset(g_t_roots_ms_by_thread, 0, sizeof(g_t_roots_ms_by_thread));
    memset(g_compact_build_ms_by_thread, 0, sizeof(g_compact_build_ms_by_thread));
    memset(g_compact_xfer_ms_by_thread, 0, sizeof(g_compact_xfer_ms_by_thread));
    memset(g_compact_batch_count_by_thread, 0, sizeof(g_compact_batch_count_by_thread));
    memset(g_compact_row_bytes_by_thread, 0, sizeof(g_compact_row_bytes_by_thread));
    memset(g_compact_col_bytes_by_thread, 0, sizeof(g_compact_col_bytes_by_thread));
    memset(g_c_rootnum_by_thread, 0, sizeof(g_c_rootnum_by_thread));
    memset(g_c_offset_by_thread, 0, sizeof(g_c_offset_by_thread));
    memset(g_c_roots_by_thread, 0, sizeof(g_c_roots_by_thread));
    memset(g_n_rootnum_dpu_by_thread, 0, sizeof(g_n_rootnum_dpu_by_thread));
    memset(g_n_offset_dpu_by_thread, 0, sizeof(g_n_offset_dpu_by_thread));
    memset(g_n_roots_dpu_by_thread, 0, sizeof(g_n_roots_dpu_by_thread));
    memset(g_b_rootnum_by_thread, 0, sizeof(g_b_rootnum_by_thread));
    memset(g_b_offset_by_thread, 0, sizeof(g_b_offset_by_thread));
    memset(g_b_roots_by_thread, 0, sizeof(g_b_roots_by_thread));
    memset(g_c_state_update_calls_by_thread, 0, sizeof(g_c_state_update_calls_by_thread));
    memset(g_c_state_update_dpus_by_thread, 0, sizeof(g_c_state_update_dpus_by_thread));
    memset(g_init_rowptr_ms_by_thread, 0, sizeof(g_init_rowptr_ms_by_thread));
    memset(g_init_colidx_ms_by_thread, 0, sizeof(g_init_colidx_ms_by_thread));
    memset(g_ig_cycle_sum, 0, sizeof(g_ig_cycle_sum));
    memset(g_ig_cycle_count, 0, sizeof(g_ig_cycle_count));
    memset(g_ig_cycle_max, 0, sizeof(g_ig_cycle_max));
    memset(g_dpu_cycle_sum, 0, sizeof(g_dpu_cycle_sum));
    memset(g_dpu_cycle_count, 0, sizeof(g_dpu_cycle_count));
    memset(g_dpu_cycle_max, 0, sizeof(g_dpu_cycle_max));
    memset(g_answer_collect_ms_by_thread, 0, sizeof(g_answer_collect_ms_by_thread));
    memset(g_pipeline_acquire_ms_by_thread,      0, sizeof(g_pipeline_acquire_ms_by_thread));
    memset(g_pipeline_inline_build_ms_by_thread, 0, sizeof(g_pipeline_inline_build_ms_by_thread));
    memset(g_pipeline_pop_hits_by_thread,        0, sizeof(g_pipeline_pop_hits_by_thread));
    memset(g_pipeline_pop_misses_by_thread,      0, sizeof(g_pipeline_pop_misses_by_thread));
    memset(g_last_rootnum_by_thread, 0, sizeof(g_last_rootnum_by_thread));
    memset(g_last_offset_by_thread, 0, sizeof(g_last_offset_by_thread));
    memset(g_last_start_root_by_thread, 0, sizeof(g_last_start_root_by_thread));
    memset(g_last_assign_iter_by_thread, 0, sizeof(g_last_assign_iter_by_thread));
    memset(g_dpu_dbg, 0, sizeof(g_dpu_dbg));

    const uint32_t n_worker_threads = MAX_N_RANK;
    pthread_t    th[MAX_N_THREAD];
    thread_args* ta[MAX_N_THREAD];
    for (uint32_t i = 0; i < MAX_N_RANK; ++i) {
        ta[i] = (thread_args*)malloc(sizeof(thread_args));
        ta[i]->thread_id = i;
        ta[i]->rank = OFIS_get_rank(set, i);
    }
#ifdef OFIS_DPU
    {
#ifdef OFIS_COMPACT_PIPELINE
        if (g_use_batch_compact) {
            compact_pipeline_init(OFIS_COMPACT_PIPELINE_QUEUE_DEPTH,
                                  OFIS_COMPACT_PIPELINE_PRODUCERS);
        }
#endif
#ifdef OFIS_PREBUILD_ALL
        ofis_prebuild_all();
#endif
#ifdef OFIS_STARTUP_BARRIER
        pthread_barrier_init(&g_startup_barrier, NULL, n_worker_threads);
#endif
        gettimeofday(&g_parallel_t0, NULL);   /* shared t0 for per-rank first-launch */
        for (uint32_t i = 0; i < n_worker_threads; ++i) {
            int rc = pthread_create(&th[i], NULL, thread_dpu_fct, (void *)ta[i]);
            if (rc != 0) {
                printf(ANSI_COLOR_RED "[OFIS] pthread_create failed at %u (rc=%d)\n" ANSI_COLOR_RESET, i, rc);
                exit(1);
            }
        }
        uint8_t joined[MAX_N_THREAD] = {0};
        uint32_t remaining = n_worker_threads;
        uint64_t last_dbg_print_ms = dbg_now_ms();

        while (remaining > 0) {
            for (uint32_t i = 0; i < n_worker_threads; ++i) {
                if (joined[i]) continue;
                int rc = pthread_tryjoin_np(th[i], NULL);
                if (rc == 0) {
                    joined[i] = 1;
                    remaining--;
                } else if (rc != EBUSY) {
                    printf(ANSI_COLOR_RED "[OFIS] pthread_tryjoin_np failed at %u (rc=%d)\n" ANSI_COLOR_RESET, i, rc);
                    exit(1);
                }
            }

            const uint64_t now_ms = dbg_now_ms();
            if (remaining > 0 && now_ms - last_dbg_print_ms >= 2000ULL) {
                last_dbg_print_ms = now_ms;
            }
            usleep(1000);
        }
#ifdef DPU_TIME_PROFILE
        // Per-DPU kernel wall (summed over on-demand refills) emitted as "DPUK <gid> <kernel_ms> <idle> <xfer> <launches>".
        for (uint32_t _r = 0; _r < n_worker_threads; ++_r)
            for (uint32_t _i = 0; _i < 64; ++_i)
                printf("DPUK %u %.3f %.3f %.3f %lu\n",
                       _r * 64u + _i, g_dpu_cycle_sum[_r][_i], 0.0, 0.0,
                       (unsigned long)g_dpu_cycle_count[_r][_i]);
        fflush(stdout);
#endif
#ifdef OFIS_COLLECT_PER_BATCH
        collect_settled_reread(set);
        ofis_collect_finalize();
#endif
#ifdef OFIS_COMPACT_PIPELINE
        if (g_pipeline.initialized) {
            compact_pipeline_destroy();
        }
#endif
#ifdef OFIS_PREBUILD_ALL
        ofis_prebuild_free();
#endif
    }
#endif

#ifdef OFIS_HYBRID
    {
        // g_split must be known before any producer/consumer pull.
#ifdef OFIS_COMPACT_PIPELINE
        if (g_use_batch_compact) {
            compute_hybrid_split();
            compact_pipeline_init(OFIS_COMPACT_PIPELINE_QUEUE_DEPTH,
                                  OFIS_COMPACT_PIPELINE_PRODUCERS);
        }
#endif
#ifdef OFIS_PREBUILD_ALL
        ofis_prebuild_all();
#endif
#ifdef OFIS_STARTUP_BARRIER
        pthread_barrier_init(&g_startup_barrier, NULL, n_worker_threads);
#endif
        gettimeofday(&g_parallel_t0, NULL);   /* shared t0 for per-rank first-launch */
        for (uint32_t i = 0; i < n_worker_threads; ++i) {
            int rc = pthread_create(&th[i], NULL, thread_hybrid_fct, (void *)ta[i]);
            if (rc != 0) {
                printf(ANSI_COLOR_RED "[OFIS] pthread_create failed at %u (rc=%d)\n" ANSI_COLOR_RESET, i, rc);
                exit(1);
            }
        }
        for (uint32_t i = 0; i < n_worker_threads; ++i) pthread_join(th[i], NULL);
#ifdef OFIS_COLLECT_PER_BATCH
        collect_settled_reread(set);
        ofis_collect_finalize();
#endif
#ifdef OFIS_COMPACT_PIPELINE
        if (g_pipeline.initialized) compact_pipeline_destroy();
#endif
#ifdef OFIS_PREBUILD_ALL
        ofis_prebuild_free();
#endif
    }
#endif
#ifdef PERF
    // Per-DPU hardware cycle load (cycle_ct); field 3 of a DPUK line is cycles here, not ms.
    for (uint32_t _r = 0; _r < MAX_N_RANK; ++_r)
        for (uint32_t _i = 0; _i < 64; ++_i)
            printf("DPUK %u %lu 0 0 0\n", _r * 64u + _i, (unsigned long)g_dpu_cycle_ct[_r][_i]);
    fflush(stdout);
#endif

    // collect mutex time
    double max_wait_ms = 0.0, max_hold_ms = 0.0;
    double sum_wait_ms = 0.0, sum_hold_ms = 0.0;
    uint64_t total_acqs = 0;

    for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) {
        if (g_pcount_lock_wait_ms_by_thread[tid] > max_wait_ms)
            max_wait_ms = g_pcount_lock_wait_ms_by_thread[tid];
        if (g_pcount_cs_hold_ms_by_thread[tid]  > max_hold_ms)
            max_hold_ms = g_pcount_cs_hold_ms_by_thread[tid];

        sum_wait_ms += g_pcount_lock_wait_ms_by_thread[tid];
        sum_hold_ms += g_pcount_cs_hold_ms_by_thread[tid];
        total_acqs  += g_pcount_lock_acqs_by_thread[tid];
    }


#ifdef OFIS_DPU
#endif

#ifdef OFIS_COMPACT_PIPELINE
    if (g_use_batch_compact) {
        double max_acq = 0.0, sum_acq = 0.0;
        double max_inline_build = 0.0, sum_inline_build = 0.0;
        uint64_t sum_hits = 0, sum_misses = 0;
        for (uint32_t tid = 0; tid < MAX_N_THREAD; ++tid) {
            const double a = g_pipeline_acquire_ms_by_thread[tid];
            const double b = g_pipeline_inline_build_ms_by_thread[tid];
            sum_acq += a;
            sum_inline_build += b;
            if (a > max_acq) max_acq = a;
            if (b > max_inline_build) max_inline_build = b;
            sum_hits   += g_pipeline_pop_hits_by_thread[tid];
            sum_misses += g_pipeline_pop_misses_by_thread[tid];
        }
        double max_prod_build = 0.0, sum_prod_build = 0.0;
        uint64_t sum_prod_builds = 0;
        for (uint32_t p = 0; p < MAX_N_THREAD; ++p) {
            const double d = g_pipeline_producer_build_ms_by_producer[p];
            sum_prod_build += d;
            if (d > max_prod_build) max_prod_build = d;
            sum_prod_builds += g_pipeline_producer_build_count_by_producer[p];
        }
        const uint64_t total = sum_hits + sum_misses;
        const double hit_pct = (total > 0) ? (100.0 * (double)sum_hits / (double)total) : 0.0;
        const double total_build_ms = sum_inline_build + sum_prod_build;
        printf("\n=== Compact Pipeline ===\n");
        printf("  pop hits     = %" PRIu64 "  (%.2f%%)\n", sum_hits, hit_pct);
        printf("  pop misses   = %" PRIu64 "  (inline fallback path)\n", sum_misses);
        printf("  acquire time         : max=%.3f ms, sum=%.3f ms\n", max_acq, sum_acq);
        printf("  inline build (miss)  : max=%.3f ms, sum=%.3f ms\n", max_inline_build, sum_inline_build);
        printf("  producer build (hit) : max=%.3f ms, sum=%.3f ms, builds=%" PRIu64 "\n",
               max_prod_build, sum_prod_build, sum_prod_builds);
        printf("  TOTAL build (inline+producer) = %.3f ms  [compare vs OFF compact_build]\n",
               total_build_ms);
        printf("========================\n");
        if (fp_time_tf) {
            fprintf(fp_time_tf, "[pipeline] hits=%" PRIu64 " misses=%" PRIu64 " hit_pct=%.2f%% "
                    "max_acq_ms=%.3f sum_acq_ms=%.3f max_inline_build_ms=%.3f sum_inline_build_ms=%.3f "
                    "max_producer_build_ms=%.3f sum_producer_build_ms=%.3f producer_builds=%" PRIu64 " "
                    "total_build_ms=%.3f\n",
                    sum_hits, sum_misses, hit_pct,
                    max_acq, sum_acq, max_inline_build, sum_inline_build,
                    max_prod_build, sum_prod_build, sum_prod_builds,
                    total_build_ms);
        }
    }
#endif

    report_scaling_bottleneck(fp_time_tf);
    fflush(stdout);

    gettimeofday(&end_time, NULL);
    double ofis_op_ms = get_time_in_milliseconds(&start_time, &end_time);
#ifdef OFIS_COLLECT_PER_BATCH
    // Exclude the settled re-read (measurement backstop, not the dispatch loop).
    ofis_op_ms -= g_collect_reread_ms;
#endif
    printf("OFIS operation Time (ms): %.6f\n", ofis_op_ms);
    fprintf(fp_time_tf, "OFIS operation Time (ms): %.6f\n", ofis_op_ms);
#ifdef GSDPU_BIN_THRESHOLD
    /* Averaged over active rank threads, so the times are comparable to E2E. */
    {
        double ms[2][2] = {{0.0, 0.0}, {0.0, 0.0}};   /* [transfer, collect][heavy, light] */
        uint64_t nb[2][2] = {{0, 0}, {0, 0}};
        uint32_t active[2] = {0, 0};
        for (uint32_t t = 0; t < MAX_N_THREAD; ++t) {
            if (g_gsxfer_ms[0][t] > 0.0 || g_gsxfer_ms[1][t] > 0.0) active[0]++;
            if (g_gscollect_ms[0][t] > 0.0 || g_gscollect_ms[1][t] > 0.0) active[1]++;
            for (int b = 0; b < 2; ++b) {
                ms[0][b] += g_gsxfer_ms[b][t];    nb[0][b] += g_gsxfer_nb[b][t];
                ms[1][b] += g_gscollect_ms[b][t]; nb[1][b] += g_gscollect_nb[b][t];
            }
        }
        const char *tag[2] = { "GSXFER", "GSCOLLECT" };
        for (int k = 0; k < 2; ++k) {
            const double div = active[k] ? (double)active[k] : 1.0;
            printf("%s light(deg<%d)=%.3f heavy=%.3f ms (avg over %u threads); "
                   "light_batches=%" PRIu64 " heavy_batches=%" PRIu64 "\n",
                   tag[k], (int)GSDPU_BIN_THRESHOLD, ms[k][1] / div, ms[k][0] / div, active[k],
                   nb[k][1], nb[k][0]);
        }
    }
#endif

    fclose(fp_time_tf);
    g_use_batch_compact = 0;
}
