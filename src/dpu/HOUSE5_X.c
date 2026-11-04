#include <dpu_mine.h>

// Schedule: E(root) -> D(second_root) -> B(third_root) -> A,C combination.
static ans_t __imp_house5_x_2(sysname_t tasklet_id, node_t root, node_t second_root,
                              node_t start, node_t step) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans = 0;

    // SA = N(E) & N(D) : hub-node B candidates; kept in slot 1 for the whole j-loop
    node_t SA_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        &col_idx[root_begin], root_end - root_begin,
        mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

    // Branch-level collaboration: split the B-candidate loop across tasklets.
    if (SA_size) for (node_t j = start; j < SA_size; j += step) {
        node_t third_root = mram_buf[tasklet_id + NR_TASKLETS][j];  // node B
        edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA

        // SD = N(B) & N(E) : roof-node A candidates (structurally contains D).
        node_t SD_size = intersect_seq_buf_thresh(tasklet_buf,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            &col_idx[root_begin], root_end - root_begin,
            mram_buf[tasklet_id], INVALID_NODE);
        if (!SD_size) continue;

        // SE = N(B) & N(D) : floor-node C candidates (structurally contains E).
        node_t SE_size = intersect_seq_buf_thresh(tasklet_buf,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            &col_idx[second_root_begin], second_root_end - second_root_begin,
            mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);
        if (!SE_size) continue;

        // common = SD-set & SE-set : the A==C coincidence (4-node trap) to subtract.
        node_t common_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], SD_size,
            mram_buf[tasklet_id + (NR_TASKLETS << 1)], SE_size,
            mram_buf[tasklet_id + 3 * NR_TASKLETS], INVALID_NODE);

        // inclusion-exclusion: SD-1 removes D from A, SE-1 removes E from C, minus A==C overlap
        ans += ((ans_t)(SD_size - 1)) * (SE_size - 1) - common_size;
    }
    return ans;
}

// Sequential (light-root) path: one tasklet handles the whole root.
static ans_t __imp_house5_x(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i];  // intended DMA
        if (second_root >= root) break;   // symmetry break (D < E)
        ans += __imp_house5_x_2(tasklet_id, root, second_root, 0, 1);
    }
    return ans;
}

#if defined(HOUSE5_X_DYNAMIC)
#include <mutex.h>
#define HOUSE5_X_WINDOW NR_TASKLETS
MUTEX_INIT(house5_x_ondemand_mutex);
MUTEX_INIT(house5_x_dyn_light_mutex);
static node_t          house5_x_shared_third_idx[HOUSE5_X_WINDOW]; /* per-window SA(hub-B) cursor */
static volatile node_t house5_x_shared_light_idx[2];               /* dynamic light-root cursor */

// On-demand heavy variant: tasklets cooperatively pull B-candidate (SA) indices from a per-second_root shared cursor.
static ans_t __imp_house5_x_2_coop_window(sysname_t tasklet_id, node_t root, node_t second_root,
                                          int window_idx) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];
    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans = 0;

    node_t SA_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        &col_idx[root_begin], root_end - root_begin,
        mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);
    if (!SA_size) return 0;

    while (1) {
        node_t j;
        mutex_lock(house5_x_ondemand_mutex);
        j = house5_x_shared_third_idx[window_idx];
        if (j < SA_size) house5_x_shared_third_idx[window_idx] = j + 1;
        mutex_unlock(house5_x_ondemand_mutex);
        if (j >= SA_size) break;

        node_t third_root = mram_buf[tasklet_id + NR_TASKLETS][j];  // node B
        edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA

        node_t SD_size = intersect_seq_buf_thresh(tasklet_buf,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            &col_idx[root_begin], root_end - root_begin,
            mram_buf[tasklet_id], INVALID_NODE);
        if (!SD_size) continue;

        node_t SE_size = intersect_seq_buf_thresh(tasklet_buf,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            &col_idx[second_root_begin], second_root_end - second_root_begin,
            mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);
        if (!SE_size) continue;

        node_t common_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], SD_size,
            mram_buf[tasklet_id + (NR_TASKLETS << 1)], SE_size,
            mram_buf[tasklet_id + 3 * NR_TASKLETS], INVALID_NODE);

        ans += ((ans_t)(SD_size - 1)) * (SE_size - 1) - common_size;
    }
    return ans;
}
#endif

extern void house5_x(sysname_t tasklet_id) {
    static ans_t partial_ans[NR_TASKLETS];
    static uint64_t partial_cycle[NR_TASKLETS];
    static uint64_t light_partial[NR_TASKLETS];   /* light-phase per-tasklet cycles (for TASKLET_IDLE) */
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;
    node_t i = 0;
    while (i < root_num) {
        node_t root = roots[i];  // intended DMA
        node_t root_begin = row_ptr[root];  // intended DMA
        node_t root_end = row_ptr[root + 1];  // intended DMA
        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }

        barrier_wait(&co_barrier);
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        partial_ans[tasklet_id] = 0;
#if !defined(HOUSE5_X_DYNAMIC)
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t second_root = col_idx[j];  // intended DMA
            if (second_root >= root) break;   // symmetry break (D < E)
            partial_ans[tasklet_id] += __imp_house5_x_2(tasklet_id, root, second_root, tasklet_id, NR_TASKLETS);
        }
#else
        // Dynamic: process second_roots in HOUSE5_X_WINDOW-sized windows sharing per-second_root SA cursors, barriers keep windows lock-step.
        {
            int finished = 0;
            for (edge_ptr chunk_start = root_begin; chunk_start < root_end; chunk_start += HOUSE5_X_WINDOW) {
                if (finished) break;
                barrier_wait(&co_barrier);
                if (tasklet_id == 0)
                    for (int k = 0; k < HOUSE5_X_WINDOW; k++) house5_x_shared_third_idx[k] = 0;
                barrier_wait(&co_barrier);
                for (int k = 0; k < HOUSE5_X_WINDOW && (chunk_start + (edge_ptr)k) < root_end; k++) {
                    node_t second_root = col_idx[chunk_start + k];  // intended DMA
                    if (second_root >= root) { finished = 1; break; }  // symmetry break (D < E)
                    partial_ans[tasklet_id] += __imp_house5_x_2_coop_window(tasklet_id, root, second_root, k);
                }
            }
        }
#endif
#ifdef PERF
        partial_cycle[tasklet_id] = timer_stop(&cycles[tasklet_id]);
#endif
        barrier_wait(&co_barrier);
        if (tasklet_id == 0) {
            ans_t total_ans = 0;
#ifdef PERF
            uint64_t _mx = 0, _sm = 0;   /* reduce over tasklets: SUM (default) or IDLE=sum(max-each) */
#endif
            for (uint32_t j = 0; j < NR_TASKLETS; j++) {
                total_ans += partial_ans[j];
#ifdef PERF
                _sm += partial_cycle[j];
                if (partial_cycle[j] > _mx) _mx = partial_cycle[j];
#endif
            }
            ANS_WRITE(i, total_ans);  // intended DMA
#ifdef PERF
#ifdef TASKLET_IDLE
            { uint64_t _idle = 0;   /* per-root tasklet idle = sum_j(max-partial); per-term, no underflow */
              for (uint32_t j = 0; j < NR_TASKLETS; j++)
                  if (partial_cycle[j] < _mx) _idle += _mx - partial_cycle[j];
              cycle_ct[cur_offset + i] = _idle; }  // intended DMA
#else
            cycle_ct[cur_offset + i] = _sm;  // intended DMA
#endif
#endif
        }
        i++;
    }

#ifdef HOUSE5_X_DYNAMIC
    // Dynamic light tail: light roots (deg<BRANCH) pulled whole per tasklet via a mutex.
    barrier_wait(&co_barrier);
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
    if (tasklet_id == 0) house5_x_shared_light_idx[0] = i;
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    barrier_wait(&co_barrier);
    while (1) {
        node_t current_i;
        mutex_lock(house5_x_dyn_light_mutex);
            current_i = house5_x_shared_light_idx[0];
            house5_x_shared_light_idx[0]++;
        mutex_unlock(house5_x_dyn_light_mutex);
        if (current_i >= root_num) break;
        node_t root = roots[current_i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(current_i, __imp_house5_x(tasklet_id, root));  // intended DMA
#ifdef PERF
        { uint64_t _c = timer_stop(&cycles[tasklet_id]);
#ifdef TASKLET_IDLE
          light_partial[tasklet_id] += _c;               /* accumulate; per-batch idle reduced below */
          cycle_ct[cur_offset + current_i] = 0;
#else
          cycle_ct[cur_offset + current_i] = _c;  // intended DMA
#endif
        }
#endif
    }
#if defined(PERF) && defined(TASKLET_IDLE)
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {   /* light-phase idle = sum_j(max-each) */
        uint64_t _mx = 0, _idle = 0;
        for (uint32_t j = 0; j < NR_TASKLETS; j++) if (light_partial[j] > _mx) _mx = light_partial[j];
        for (uint32_t j = 0; j < NR_TASKLETS; j++) if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
        cycle_ct[cur_offset + light_start] = _idle;
    }
#endif
#else
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    for (i += tasklet_id; i < root_num; i += NR_TASKLETS) {
        node_t root = roots[i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(i, __imp_house5_x(tasklet_id, root));  // intended DMA
#ifdef PERF
        { uint64_t _c = timer_stop(&cycles[tasklet_id]);
#ifdef TASKLET_IDLE
          light_partial[tasklet_id] += _c;
          cycle_ct[cur_offset + i] = 0;
#else
          cycle_ct[cur_offset + i] = _c;  // intended DMA
#endif
        }
#endif
    }
#if defined(PERF) && defined(TASKLET_IDLE)
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {   /* static light-phase idle = sum_j(max-each) */
        uint64_t _mx = 0, _idle = 0;
        for (uint32_t j = 0; j < NR_TASKLETS; j++) if (light_partial[j] > _mx) _mx = light_partial[j];
        for (uint32_t j = 0; j < NR_TASKLETS; j++) if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
        cycle_ct[cur_offset + light_start] = _idle;
    }
#endif
#endif
    ans_flush(tasklet_id);
}
