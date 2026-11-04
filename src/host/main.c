#include <common.h>
#include <timer.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>

#include <dpu.h>
#include <ofis.h>



extern void data_transfer(struct dpu_set_t set, Graph *g);
extern void   pimpam_iter_run(struct dpu_set_t set, Graph *g);
extern ans_t  g_pimpam_iter_total;
extern double g_pimpam_iter_transfer_ms;
extern double g_pimpam_iter_dpu_ms;
extern ans_t clique2(Graph *g, node_t root);
extern ans_t KERNEL_FUNC(Graph *g, node_t root);
extern int OFIS_answers_collected_in_worker_threads(void);
extern double OFIS_get_answer_collect_max_ms(void);
extern double OFIS_get_answer_collect_mean_ms(void);
extern double OFIS_get_answer_collect_sum_ms(void);
extern uint64_t OFIS_get_answer_collect_calls(void);
extern uint64_t OFIS_get_answer_collect_rounds(void);
Graph *g;
ans_t ans[N];
ans_t result[N];
Timer timer;
Timer collect_timer;
uint64_t cycle_ct[N];
uint64_t cycle_ct_dpu[NR_DPUS][NR_TASKLETS];


#ifdef PERF
// Per-DPU load-balance report (PimPam + OFIS): per-root weight sum, CV/max-mean.
#define LB_CYCLES_PER_MS 350000.0

typedef struct lb_stat_t {
    uint32_t dpu_id;
    uint64_t root_num;
    uint64_t w;        /* per-DPU summed weight (degree or cycle) */
} lb_stat_t;

static double lb_sqrt_main(double x) {
    if (x <= 0.0) return 0.0;
    double r = x;
    for (int i = 0; i < 40; i++) {
        double n = 0.5 * (r + x / r);
        if (n == r) break;
        r = n;
    }
    return r;
}

static int lb_cmp_w_desc(const void *a, const void *b) {
    const lb_stat_t *x = (const lb_stat_t *)a;
    const lb_stat_t *y = (const lb_stat_t *)b;
    if (x->w < y->w) return 1;
    if (x->w > y->w) return -1;
    return 0;
}

// w==NULL: weight by out-degree; w!=NULL: weight by w[root]; per_ms>0: also print wall ms.
static void lb_report(const char *label, const char *metric,
                      const uint64_t *w, double per_ms) {
    const char *tag = (label && *label) ? label : "run";
    lb_stat_t *st = calloc(NR_DPUS, sizeof(*st));
    if (!st) { printf(ANSI_COLOR_RED "lb_report: alloc failed\n" ANSI_COLOR_RESET); return; }

    uint64_t total_w = 0, total_roots = 0;
    uint64_t wmin = ~0ULL, wmax = 0, cmin = ~0ULL, cmax = 0;
    uint32_t active = 0;
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        const uint64_t rn = g->root_num[i];
        st[i].dpu_id = i; st[i].root_num = rn; st[i].w = 0;
        if (rn == 0 || g->roots[i] == NULL) continue;
        uint64_t ws = 0;
        for (uint64_t k = 0; k < rn; k++) {
            const node_t r = g->roots[i][k];
            if (r >= g->n) continue;
            ws += w ? w[r] : (uint64_t)(g->row_ptr[r + 1] - g->row_ptr[r]);
        }
        st[i].w = ws;
        active++; total_w += ws; total_roots += rn;
        if (ws < wmin) wmin = ws;
        if (ws > wmax) wmax = ws;
        if (rn < cmin) cmin = rn;
        if (rn > cmax) cmax = rn;
    }
    if (active == 0) { printf("[LB:%s/%s] no active DPU — skipping\n", tag, metric); free(st); return; }

    const double mean_w = (double)total_w / active;
    const double mean_c = (double)total_roots / active;
    double var_w = 0.0, var_c = 0.0;
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        if (st[i].root_num == 0) continue;
        const double dw = (double)st[i].w - mean_w; var_w += dw * dw;
        const double dc = (double)st[i].root_num - mean_c; var_c += dc * dc;
    }
    var_w /= active; var_c /= active;
    const double cv_w = (mean_w > 0.0) ? (lb_sqrt_main(var_w) / mean_w) : 0.0;
    const double cv_c = (mean_c > 0.0) ? (lb_sqrt_main(var_c) / mean_c) : 0.0;
    const double imb_w = (mean_w > 0.0) ? ((double)wmax / mean_w) : 0.0;
    const double mm_w  = (wmin > 0)     ? ((double)wmax / (double)wmin) : 0.0;

    printf("\n=== Load Balance [%s]  weight=%s ===\n", tag, metric);
    printf("  active=%u/%u, total_roots=%lu, total_%s=%lu\n",
           active, NR_DPUS, total_roots, metric, total_w);
    printf("  root_num/DPU : min=%lu max=%lu mean=%.1f CV=%.4f max/mean=%.4f\n",
           cmin, cmax, mean_c, cv_c, (mean_c > 0.0) ? (double)cmax / mean_c : 0.0);
    if (per_ms > 0.0) {
        const double tk = (double)NR_TASKLETS;
        printf("  %s/DPU : min=%lu max=%lu mean=%.0f CV=%.4f max/mean=%.4f max/min=%.4f\n",
               metric, wmin, wmax, mean_w, cv_w, imb_w, mm_w);
        printf("  approx wall: mean=%.1f ms, makespan=%.1f ms  (sum/tasklets/%.0f)\n",
               mean_w / (tk * per_ms), (double)wmax / (tk * per_ms), per_ms);
    } else {
        printf("  %s/DPU : min=%lu max=%lu mean=%.1f CV=%.4f max/mean=%.4f max/min=%.4f\n",
               metric, wmin, wmax, mean_w, cv_w, imb_w, mm_w);
    }
    printf("  >>> uniformity: CV(%s)=%.4f, max/mean=%.4f  (lower = more uniform)\n",
           metric, cv_w, imb_w);
    printf("=========================================================\n");

    char path[512];
    snprintf(path, sizeof(path), "./result/loadbalance_%s_%s_%s_%s.csv",
             metric, tag, PATTERN_NAME, DATA_NAME);
    FILE *f = fopen(path, "w");
    if (f) {
        qsort(st, NR_DPUS, sizeof(*st), lb_cmp_w_desc);
        if (per_ms > 0.0) {
            fprintf(f, "rank,dpu_id,root_num,%s,approx_ms\n", metric);
            for (uint32_t k = 0; k < NR_DPUS; k++)
                fprintf(f, "%u,%u,%lu,%lu,%.4f\n", k, st[k].dpu_id, st[k].root_num,
                        st[k].w, (double)st[k].w / ((double)NR_TASKLETS * per_ms));
        } else {
            fprintf(f, "rank,dpu_id,root_num,%s\n", metric);
            for (uint32_t k = 0; k < NR_DPUS; k++)
                fprintf(f, "%u,%u,%lu,%lu\n", k, st[k].dpu_id, st[k].root_num, st[k].w);
        }
        fclose(f);
        printf("[LB] per-DPU detail: %s\n", path);
    } else {
        printf(ANSI_COLOR_RED "[LB] cannot write %s\n" ANSI_COLOR_RESET, path);
    }
    free(st);
}

// Dump ground-truth per-root cycle cost for a later OFIS run to weight by. Keyed by PATTERN/DATA.
static void lb_dump_rootcycles(void) {
    char path[512];
    snprintf(path, sizeof(path), "./result/rootcycle_%s_%s.bin", PATTERN_NAME, DATA_NAME);
    FILE *f = fopen(path, "wb");
    if (!f) { printf(ANSI_COLOR_RED "[LB] cannot write %s\n" ANSI_COLOR_RESET, path); return; }
    const uint64_t n = (uint64_t)g->n;
    fwrite(&n, sizeof(n), 1, f);
    fwrite(cycle_ct, sizeof(uint64_t), (size_t)n, f);   /* cycle_ct indexed by node id */
    fclose(f);
    printf("[LB] ground-truth per-root cycles dumped: %s (n=%lu)\n", path, n);
}

// Per-root (degree, cycle) CSV: one row per root vertex. PERF build only.
static void lb_dump_degree_cycle_csv(void) {
    char path[512];
    snprintf(path, sizeof(path), "./result/degcycle_%s_%s.csv", PATTERN_NAME, DATA_NAME);
    FILE *f = fopen(path, "w");
    if (!f) { printf(ANSI_COLOR_RED "[LB] cannot write %s\n" ANSI_COLOR_RESET, path); return; }
    fprintf(f, "node,degree,cycle\n");
    const uint64_t n = (uint64_t)g->n;
    for (uint64_t v = 0; v < n; v++) {
        const uint64_t deg = (uint64_t)(g->row_ptr[v + 1] - g->row_ptr[v]);
        fprintf(f, "%lu,%lu,%lu\n", (unsigned long)v, (unsigned long)deg,
                (unsigned long)cycle_ct[v]);
    }
    fclose(f);
    printf("[LB] per-root (degree,cycle) dumped: %s (n=%lu, cycles/ms=%g)\n",
           path, n, (double)LB_CYCLES_PER_MS);
}

static uint64_t *lb_load_rootcycles(void) {
    char path[512];
    snprintf(path, sizeof(path), "./result/rootcycle_%s_%s.bin", PATTERN_NAME, DATA_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf(ANSI_COLOR_RED
               "[LB] ground-truth cycle file missing: %s\n"
               "     Run the STATIC (non-OFIS) build of this workload once first;\n"
               "     skipping cycle-weighted (Method A) report.\n" ANSI_COLOR_RESET, path);
        return NULL;
    }
    uint64_t n = 0;
    if (fread(&n, sizeof(n), 1, f) != 1 || n != (uint64_t)g->n) {
        printf(ANSI_COLOR_RED "[LB] cycle file n mismatch (%lu vs %u) — skipping\n"
               ANSI_COLOR_RESET, n, g->n);
        fclose(f); return NULL;
    }
    uint64_t *w = malloc((size_t)n * sizeof(uint64_t));
    if (!w || fread(w, sizeof(uint64_t), (size_t)n, f) != n) {
        printf(ANSI_COLOR_RED "[LB] cycle file read error\n" ANSI_COLOR_RESET);
        free(w); fclose(f); return NULL;
    }
    fclose(f);
    return w;
}
#endif /* PERF */


int main() {
    printf("NR_DPUS: %u, NR_TASKLETS: %u, DPU_BINARY: %s, PATTERN: %s\n", NR_DPUS, NR_TASKLETS, DPU_BINARY, PATTERN_NAME);

#ifdef DEGCYCLE_CPU
    // CPU-only per-root timing: no DPU alloc. Runs and exits.
    { extern void degcycle_cpu_run(Graph *g); Graph *gg = malloc(sizeof(Graph));
      degcycle_cpu_run(gg); return 0; }
#endif


    struct dpu_set_t set, dpu;

    DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, &set));
#ifdef OFIS_MODE
    DPU_ASSERT(dpu_load(set, DPU_BINARY, NULL));
#endif
    
    ans_t total_ans = 0;
    // collect answer and cycle count
    bool fine = true;
    bool finished, failed;
    uint32_t each_dpu;
    bool collect_in_worker_threads = false;
    double transfer_ms = 0.0;
    double dpu_ms = 0.0;
    double collect_ms = 0.0;

#ifndef OFIS_MODE

    // task allocation and data partition. pimpam_iter_run runs the static pipeline
    // (allocate + compact + launch + collect into result[]) in one round when the graph
    // fits 1024 DPUs, or in more rounds when it overflows. The answer is summed there,
    // so main's single-pass launch + collect below are skipped for the static path.
    printf("Selecting graph: %s\n", DATA_PATH);
    g = malloc(sizeof(Graph));
    pimpam_iter_run(set, g);
    transfer_ms = g_pimpam_iter_transfer_ms;
    dpu_ms      = g_pimpam_iter_dpu_ms;
    total_ans   = g_pimpam_iter_total;
    printf("PimPam transfer %.3f ms, dpu %.3f ms\n", transfer_ms, dpu_ms);
#endif  /* !OFIS_MODE */
#ifdef OFIS_MODE

// task allocation and data partition
    
    printf("Selecting graph: %s\n", DATA_PATH);
    start(&timer, 0, 0);
    g = malloc(sizeof(Graph));
    OFIS_data_transfer(set, g);
    stop(&timer, 0);
    transfer_ms = timer.time[0] / 1000.0;
    printf("OFIS Processing(total) ");
    print(&timer, 0, 1);
    collect_in_worker_threads = OFIS_answers_collected_in_worker_threads();
#endif
       
    // run it on CPU to get the answer
#ifdef CPU_RUN
    start(&timer, 0, 0);
    for (node_t i = 0; i < g->n; i++) {
        ans[i] = KERNEL_FUNC(g, i);
        total_ans += ans[i];
    }
    stop(&timer, 0);
    printf("CPU ");
    print(&timer, 0, 1);
    printf("CPU ans: %lu\n", total_ans);
    total_ans = 0;
#endif  // CPU_RUN
#ifdef PERF
    uint64_t total_cycle_ct = 0;
#endif
    // DPU fault check outside the collection timer; stop at first failing DPU.
    uint32_t first_failed_dpu = NR_DPUS;
    DPU_FOREACH(set, dpu, each_dpu) {
        DPU_ASSERT(dpu_status(dpu, &finished, &failed));
        if (failed) {
            printf("DPU: %u failed\n", each_dpu);
            fine = false;
            first_failed_dpu = each_dpu;
            break;
        }
    }

    if (!collect_in_worker_threads) {
        start(&collect_timer, 0, 0);
    }
#ifdef OFIS_MODE   /* static PimPam already collected in pimpam_iter_run; only OFIS collects here */
#ifdef PIMPAM_COLLECT_PARALLEL
    // Parallel final collect: pull every DPU's "ans" in one rank-wide xfer, padded to max root count.
    uint64_t  pim_max_rn  = 0;
    uint64_t *pim_ans_all = NULL;
    if (!collect_in_worker_threads) {
        DPU_FOREACH(set, dpu, each_dpu) {
            if (g->root_num[each_dpu] > pim_max_rn) pim_max_rn = g->root_num[each_dpu];
        }
        if (pim_max_rn == 0) pim_max_rn = 1;
        pim_ans_all = (uint64_t *)malloc((size_t)NR_DPUS * pim_max_rn * sizeof(uint64_t));
        if (!pim_ans_all) { perror("pim_ans_all alloc"); exit(1); }
        DPU_FOREACH(set, dpu, each_dpu) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, pim_ans_all + (uint64_t)each_dpu * pim_max_rn));
        }
        DPU_ASSERT(dpu_push_xfer(set, DPU_XFER_FROM_DPU, "ans", 0,
                                 ALIGN8(pim_max_rn * sizeof(uint64_t)), DPU_XFER_DEFAULT));
    }
#endif
    DPU_FOREACH(set, dpu, each_dpu) {
        // status was checked before the timer started; stop where it failed.
        if (each_dpu >= first_failed_dpu) break;

#ifdef OFIS_MODE
        if (collect_in_worker_threads) {
            for (node_t k = 0, cur_thread = 0; k < g->root_num[each_dpu]; k++) {
                node_t cur_root = g->roots[each_dpu][k];
                total_ans += result[cur_root];
#ifdef CPU_RUN
                if (ans[cur_root] != result[cur_root]) {
                    printf("Wrong answer at dpu %u node %u: %lu != %lu\n", each_dpu, cur_root, ans[cur_root], result[cur_root]);
                    fine = false;
                }
#endif
#ifdef PERF
                if (g->row_ptr[cur_root + 1] - g->row_ptr[cur_root] >= BRANCH_LEVEL_THRESHOLD) {
                    for (uint32_t i = 0; i < NR_TASKLETS; i++) {
                        cycle_ct_dpu[each_dpu][i] += cycle_ct[cur_root] / NR_TASKLETS;
                    }
                    total_cycle_ct += cycle_ct[cur_root];
                }
                else {
                    cycle_ct_dpu[each_dpu][cur_thread] += cycle_ct[cur_root];
                    cur_thread = (cur_thread + 1) % NR_TASKLETS;
                    total_cycle_ct += cycle_ct[cur_root];
                }
#endif
            }
            continue;
        }
#endif

        // collect answer

#ifdef PIMPAM_COLLECT_PARALLEL
        // point into the pre-fetched buffer; no per-DPU transfer
        uint64_t *dpu_ans = pim_ans_all + (uint64_t)each_dpu * pim_max_rn;
#else
        uint64_t *dpu_ans = (uint64_t *)malloc(ALIGN8(g->root_num[each_dpu] * sizeof(uint64_t)));
        DPU_ASSERT(dpu_copy_from(dpu, "ans", 0, dpu_ans, ALIGN8(g->root_num[each_dpu] * sizeof(uint64_t))));
#endif
        for (node_t k = 0; k < g->root_num[each_dpu]; k++) {
            node_t cur_root = g->roots[each_dpu][k];
            result[cur_root] = dpu_ans[k];
            total_ans += dpu_ans[k];
#ifdef CPU_RUN
            if (ans[cur_root] != dpu_ans[k]) {
                printf("Wrong answer at dpu %u node %u: %lu != %lu\n", each_dpu, cur_root, ans[cur_root], dpu_ans[k]);
#ifdef DPU_LOG
                DPU_ASSERT(dpu_log_read(dpu, stdout));
#endif
                fine = false;
            }
#endif  // CPU_RUN
        }
#ifndef PIMPAM_COLLECT_PARALLEL
        free(dpu_ans);
#endif

        // collect cycle count
#ifdef PERF
        uint64_t *dpu_cycle_ct = (uint64_t *)malloc(ALIGN8(g->root_num[each_dpu] * sizeof(uint64_t)));
        DPU_ASSERT(dpu_copy_from(dpu, "cycle_ct", 0, dpu_cycle_ct, ALIGN8(g->root_num[each_dpu] * sizeof(uint64_t))));
        for (node_t k = 0, cur_thread = 0; k < g->root_num[each_dpu]; k++) {
            node_t cur_root = g->roots[each_dpu][k];
            cycle_ct[cur_root] = dpu_cycle_ct[k];
            if (g->row_ptr[cur_root + 1] - g->row_ptr[cur_root] >= BRANCH_LEVEL_THRESHOLD) {
                for (uint32_t i = 0; i < NR_TASKLETS; i++) {
                    cycle_ct_dpu[each_dpu][i] += dpu_cycle_ct[k] / NR_TASKLETS;
                }
                total_cycle_ct += dpu_cycle_ct[k];
            }
            else {
                cycle_ct_dpu[each_dpu][cur_thread] += dpu_cycle_ct[k];
                cur_thread = (cur_thread + 1) % NR_TASKLETS;
                total_cycle_ct += dpu_cycle_ct[k];
            }
        }
        free(dpu_cycle_ct);
#endif
    }
#ifdef PIMPAM_COLLECT_PARALLEL
    free(pim_ans_all);
#endif
#endif  /* OFIS_MODE */
    if (!collect_in_worker_threads) {
        stop(&collect_timer, 0);
        collect_ms = collect_timer.time[0] / 1000.0;
    } else {
        collect_ms = OFIS_get_answer_collect_max_ms();
    }

    printf("DPU ans: %lu\n", total_ans);
    printf("Answer collecting ");
    if (collect_in_worker_threads) {
        printf("Time (ms): %f\n", collect_ms);
        // max = critical-rank wall; mean = per-rank wall (sum/active-ranks); sum = total work
        printf("Answer collecting mean (ms): %f\n", OFIS_get_answer_collect_mean_ms());
        printf("Answer collecting sum (ms): %f\n",  OFIS_get_answer_collect_sum_ms());
        // transfer-count summary for the collect-nbatch figure; `calls` = total DPU-batches
        // collected (= task batches under per-batch collect), `rounds` = push operations.
        printf("  collect rounds=%lu calls=%lu\n",
               (unsigned long)OFIS_get_answer_collect_rounds(),
               (unsigned long)OFIS_get_answer_collect_calls());
    } else {
        print(&collect_timer, 0, 1);
    }
    // End-to-end = transfer + execute + collect (disjoint); subtract the per-batch settled re-read (a correctness backstop inside transfer_ms) back out.
    double reread_ms = 0.0;
#ifdef OFIS_COLLECT_PER_BATCH
    extern double get_collect_reread_ms(void);
    reread_ms = get_collect_reread_ms();
#endif
    printf("End-to-end ");
    printf("Time (ms): %f\n",
           transfer_ms + dpu_ms - reread_ms + (collect_in_worker_threads ? 0.0 : collect_ms));
#ifdef PERF
    printf("Lower bound: %f\n", (double)total_cycle_ct / NR_DPUS / NR_TASKLETS / 350000);
#endif

#ifdef PERF
    // Load balance: degree (a-priori proxy) vs cycle (ground-truth per-root cost).
#ifdef OFIS_MODE
    {
        const char *tag = "ofis_pimpam";
        lb_report(tag, "degree", NULL, 0.0);
        uint64_t *w = lb_load_rootcycles();          /* ground truth from a prior static run */
        if (w) { lb_report(tag, "cycle", w, LB_CYCLES_PER_MS); free(w); }
    }
#else
    {
        const char *tag = "pimpam";
        lb_report(tag, "degree", NULL, 0.0);
        lb_dump_rootcycles();                         /* ground truth for later OFIS runs */
        lb_report(tag, "cycle", cycle_ct, LB_CYCLES_PER_MS);
    }
#endif
    // Per-root (degree,cycle) CSV for the load-vs-degree plot. Runs in both modes.
    lb_dump_degree_cycle_csv();
#endif

    // output result to file
#ifdef PERF
    FILE *fp = fopen("./result/" PATTERN_NAME "_" DATA_NAME ".txt", "w");
    fprintf(fp, "NR_DPUS: %u, NR_TASKLETS: %u, DPU_BINARY: %s, PATTERN: %s\n", NR_DPUS, NR_TASKLETS, DPU_BINARY, PATTERN_NAME);
    fprintf(fp, "N: %u, M: %u, avg_deg: %f\n", g->n, g->m, (double)g->m / g->n);
    for (node_t i = 0; i < g->n; i++) {
        fprintf(fp, "node: %u, deg: %u, o_deg: %lu, ans: %lu, cycle: %lu,\n", i, g->row_ptr[i + 1] - g->row_ptr[i], clique2(g, i), result[i], cycle_ct[i]);
    }
    for (uint32_t i = 0; i < NR_DPUS; i++) {
        for (uint32_t j = 0; j < NR_TASKLETS; j++) {
            fprintf(fp, "DPU: %u, tasklet: %u, cycle: %lu, root_num: %lu\n", i, j, cycle_ct_dpu[i][j], g->root_num[i]);
        }
    }
    fclose(fp);
#endif
    if (fine) printf(ANSI_COLOR_GREEN "All fine\n" ANSI_COLOR_RESET);
    else printf(ANSI_COLOR_RED "Some failed\n" ANSI_COLOR_RESET);

    free(g);
    DPU_ASSERT(dpu_free(set));
    return 0;
}
