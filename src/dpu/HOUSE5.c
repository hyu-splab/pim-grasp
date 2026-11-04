#include <dpu_mine.h>
#ifdef HOUSE5_DYNAMIC
#include <mutex.h>
#endif




static ans_t __imp_house5_2(sysname_t tasklet_id, node_t root, node_t second_root, node_t start, node_t step) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans = 0;
    node_t fifth_root_size = intersect_seq_buf_thresh(tasklet_buf, &col_idx[second_root_begin], second_root_end - second_root_begin, &col_idx[root_begin], root_end - root_begin, mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);
    node_t cur_cmp = 0;
    if (fifth_root_size) for (edge_ptr j = root_begin + start; j < root_end; j += step) {
        node_t third_root = col_idx[j];  // intended DMA
        if (third_root == second_root) continue;
        edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA
        node_t fourth_root_size = intersect_seq_buf_thresh(tasklet_buf, &col_idx[second_root_begin], second_root_end - second_root_begin, &col_idx[third_root_begin], third_root_end - third_root_begin, mram_buf[tasklet_id], INVALID_NODE);
        if (!fourth_root_size) continue;
        node_t common_size = intersect_seq_buf_thresh(tasklet_buf, mram_buf[tasklet_id], fourth_root_size, mram_buf[tasklet_id + NR_TASKLETS], fifth_root_size, mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);
        node_t cur_fifth = fifth_root_size;
        while (cur_cmp < fifth_root_size) {
            node_t fifth_root = mram_buf[tasklet_id + NR_TASKLETS][cur_cmp];  // intended DMA
            if (fifth_root > third_root) break;
            else if (fifth_root == third_root) {
                cur_fifth--;
                cur_cmp++;
                break;
            }
            cur_cmp++;
        }
        ans += ((ans_t)cur_fifth) * (fourth_root_size - 1) - common_size;
    }
    return ans;
}

static ans_t __imp_house5(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i];  // intended DMA
        if (second_root >= root) break;
        ans += __imp_house5_2(tasklet_id, root, second_root, 0, 1);
    }
    return ans;
}

#if defined(HOUSE5_DYNAMIC)
// Dynamic light-root cursor: light roots (deg<BRANCH) pulled one-at-a-time via a mutex.
MUTEX_INIT(house5_dyn_light_mutex);
static volatile node_t shared_light_idx[2];
#endif

#ifdef HOUSE5_DYNAMIC
// On-demand variant: tasklets pull third-root indices from per-second_root shared cursors (HOUSE5_WINDOW in flight).
#define HOUSE5_WINDOW NR_TASKLETS
MUTEX_INIT(house5_ondemand_mutex);
static edge_ptr shared_third_idx[HOUSE5_WINDOW];

static ans_t __imp_house5_2_coop_window(sysname_t tasklet_id, node_t root, node_t second_root,
                                        edge_ptr root_begin, edge_ptr root_end, int window_idx) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans = 0;
    node_t fifth_root_size = intersect_seq_buf_thresh(tasklet_buf, &col_idx[second_root_begin], second_root_end - second_root_begin, &col_idx[root_begin], root_end - root_begin, mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);
    if (!fifth_root_size) return 0;
    while (1) {
        edge_ptr j;
        mutex_lock(house5_ondemand_mutex);
        j = shared_third_idx[window_idx];
        if (j < root_end) shared_third_idx[window_idx] = j + 1;
        mutex_unlock(house5_ondemand_mutex);
        if (j >= root_end) break;
        node_t third_root = col_idx[j];  // intended DMA
        if (third_root == second_root) continue;
        edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA
        node_t fourth_root_size = intersect_seq_buf_thresh(tasklet_buf, &col_idx[second_root_begin], second_root_end - second_root_begin, &col_idx[third_root_begin], third_root_end - third_root_begin, mram_buf[tasklet_id], INVALID_NODE);
        if (!fourth_root_size) continue;
        node_t common_size = intersect_seq_buf_thresh(tasklet_buf, mram_buf[tasklet_id], fourth_root_size, mram_buf[tasklet_id + NR_TASKLETS], fifth_root_size, mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);
        node_t cur_fifth = fifth_root_size;
        node_t cur_cmp = 0;
        while (cur_cmp < fifth_root_size) {
            node_t fifth_root = mram_buf[tasklet_id + NR_TASKLETS][cur_cmp];  // intended DMA
            if (fifth_root > third_root) break;
            else if (fifth_root == third_root) { cur_fifth--; break; }
            cur_cmp++;
        }
        ans += ((ans_t)cur_fifth) * (fourth_root_size - 1) - common_size;
    }
    return ans;
}
#endif

extern void house5(sysname_t tasklet_id) {
    static ans_t partial_ans[NR_TASKLETS];

    static uint64_t partial_cycle[NR_TASKLETS];
    static uint64_t light_partial[NR_TASKLETS];   /* async light-root cycles per tasklet */
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;


    node_t i = 0;
    while (i < root_num) {
        node_t root = roots[i];  // intended DMA
        node_t root_begin = row_ptr[root];  // intended DMA
        node_t root_end = row_ptr[root + 1];  // intended DMA

        // static: heavy roots (deg>=BRANCH) cooperate here; light roots go to the
        // per-tasklet tail loop below, so break at the first light root. On-demand has
        // no tail loop and processes all roots here, so it must not break.
        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }

        barrier_wait(&co_barrier);
        partial_ans[tasklet_id] = 0;
#ifdef PERF
        partial_cycle[tasklet_id] = 0;   /* matching cycles only (barriers excluded) */
#endif
#ifndef HOUSE5_DYNAMIC
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t second_root = col_idx[j];  // intended DMA
            if (second_root >= root) break;
            partial_ans[tasklet_id] += __imp_house5_2(tasklet_id, root, second_root, tasklet_id, NR_TASKLETS);
        }
#ifdef PERF
        partial_cycle[tasklet_id] = timer_stop(&cycles[tasklet_id]);
#endif
#else
        // Dynamic: process second_roots in HOUSE5_WINDOW-sized windows sharing per-second_root cursors, barriers keep windows lock-step.
        {
            int finished = 0;
            for (edge_ptr chunk_start = root_begin; chunk_start < root_end; chunk_start += HOUSE5_WINDOW) {
                if (finished) break;
                barrier_wait(&co_barrier);
                if (tasklet_id == 0)
                    for (int k = 0; k < HOUSE5_WINDOW; k++) shared_third_idx[k] = root_begin;
                barrier_wait(&co_barrier);
                for (int k = 0; k < HOUSE5_WINDOW && (chunk_start + (edge_ptr)k) < root_end; k++) {
                    node_t second_root = col_idx[chunk_start + k];  // intended DMA
                    if (second_root >= root) { finished = 1; break; }
#ifdef PERF
                    timer_start(&cycles[tasklet_id]);
#endif
                    partial_ans[tasklet_id] += __imp_house5_2_coop_window(tasklet_id, root, second_root, root_begin, root_end, k);
#ifdef PERF
                    partial_cycle[tasklet_id] += timer_stop(&cycles[tasklet_id]);
#endif
                }
            }
        }
#endif
        barrier_wait(&co_barrier);
        if (tasklet_id == 0) {
            ans_t total_ans = 0;
#ifdef PERF
            // per-root load metric: slowest tasklet (default) or tasklet idle (TASKLET_IDLE).
            uint64_t _mx = 0;
#endif
            for (uint32_t j = 0; j < NR_TASKLETS; j++) {
                total_ans += partial_ans[j];
#ifdef PERF
                if (partial_cycle[j] > _mx) _mx = partial_cycle[j];
#endif
            }
            ANS_WRITE(i, total_ans);  // intended DMA

#ifdef PERF
#ifdef TASKLET_IDLE
            { uint64_t _idle = 0;   /* sum_j (max - partial_j), per-term to avoid underflow */
              for (uint32_t j = 0; j < NR_TASKLETS; j++)
                  if (partial_cycle[j] < _mx) _idle += _mx - partial_cycle[j];
              cycle_ct[cur_offset + i] = _idle; }  // intended DMA
#else
            cycle_ct[cur_offset + i] = _mx;  // intended DMA
#endif
#endif
        }
        i++;
    }

#ifdef HOUSE5_DYNAMIC
    // Dynamic light tail: light roots (deg<BRANCH) pulled whole per tasklet via a mutex.
    barrier_wait(&co_barrier);
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
    if (tasklet_id == 0) shared_light_idx[0] = i;
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    barrier_wait(&co_barrier);
    while (1) {
        node_t current_i;
        mutex_lock(house5_dyn_light_mutex);
            current_i = shared_light_idx[0];
            shared_light_idx[0]++;
        mutex_unlock(house5_dyn_light_mutex);
        if (current_i >= root_num) break;
        node_t root = roots[current_i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(current_i, __imp_house5(tasklet_id, root));  // intended DMA
#ifdef PERF
        light_partial[tasklet_id] += timer_stop(&cycles[tasklet_id]);
        cycle_ct[cur_offset + current_i] = 0;   /* async: WALL is per-tasklet max, set below */
#endif
    }
#ifdef PERF
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {
        uint64_t _mx = 0;   /* light phase: slowest tasklet, or idle = sum(max - each) */
        for (uint32_t j = 0; j < NR_TASKLETS; j++) {
            if (light_partial[j] > _mx) _mx = light_partial[j];
        }
#ifdef TASKLET_IDLE
        { uint64_t _idle = 0;
          for (uint32_t j = 0; j < NR_TASKLETS; j++)
              if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
          cycle_ct[cur_offset + light_start] = _idle; }
#else
        cycle_ct[cur_offset + light_start] = _mx;
#endif
    }
#endif
#else
    // Static light tail: light roots (deg<BRANCH) one whole root per tasklet, strided; on-demand skips this.
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    for (i += tasklet_id; i < root_num; i += NR_TASKLETS) {
        node_t root = roots[i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(i, __imp_house5(tasklet_id, root));  // intended DMA
#ifdef PERF
        light_partial[tasklet_id] += timer_stop(&cycles[tasklet_id]);
        cycle_ct[cur_offset + i] = 0;   /* async: WALL is per-tasklet max, set below */
#endif
    }
#ifdef PERF
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {
        uint64_t _mx = 0;   /* light phase: slowest tasklet, or idle = sum(max - each) */
        for (uint32_t j = 0; j < NR_TASKLETS; j++) {
            if (light_partial[j] > _mx) _mx = light_partial[j];
        }
#ifdef TASKLET_IDLE
        { uint64_t _idle = 0;
          for (uint32_t j = 0; j < NR_TASKLETS; j++)
              if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
          cycle_ct[cur_offset + light_start] = _idle; }
#else
        cycle_ct[cur_offset + light_start] = _mx;
#endif
    }
#endif
#endif
    ans_flush(tasklet_id);
}
