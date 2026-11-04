#include <dpu_mine.h>

// HOUSE5_Y: 5-vertex 8-edge pattern = K5 minus edges (0,2),(0,3); |Aut|=4; non-induced, matching GraphPi.

static ans_t __imp_house5_y_2(sysname_t tasklet_id, node_t root, node_t second_root,
                              node_t start, node_t step) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];          // intended DMA
    edge_ptr root_end = row_ptr[root + 1];        // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];   // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1]; // intended DMA
    ans_t ans = 0;

    // buf0 = N(second_root) & N(root): the C/D/E candidates. Kept in slot tid.
    node_t buf0_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        &col_idx[root_begin], root_end - root_begin,
        mram_buf[tasklet_id], INVALID_NODE);

    if (buf0_size < 3) return 0;   // need at least C, D, E

    // split the C-loop across tasklets for the heavy-root path (start/step)
    for (node_t j = start; j < buf0_size; j += step) {
        node_t third_root = mram_buf[tasklet_id][j];   // node C
        edge_ptr third_root_begin = row_ptr[third_root];   // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1]; // intended DMA

        // buf1 = buf0 & N(C): the D candidates (each adjacent to A,B,C). Slot tid+NR.
        node_t buf1_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], buf0_size,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

        for (node_t k = 0; k < buf1_size; k++) {
            node_t fourth_root = mram_buf[tasklet_id + NR_TASKLETS][k];   // node D
            if (fourth_root >= third_root) break;   // D < C (buf1 sorted); breaks C<->D
            ans += (buf0_size - 2);   // E = any other common neighbour (deg-2 vertex)
        }
    }
    return ans;
}

// sequential (light-root) path: one tasklet handles the whole root.
static ans_t __imp_house5_y(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];   // intended DMA
    edge_ptr root_end = row_ptr[root + 1]; // intended DMA
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i];   // intended DMA
        if (second_root >= root) break;    // B < A (deg-4 swap broken)
        ans += __imp_house5_y_2(tasklet_id, root, second_root, 0, 1);
    }
    return ans;
}

#if defined(HOUSE5_Y_DYNAMIC)
#include <mutex.h>
#define HOUSE5_Y_WINDOW NR_TASKLETS
MUTEX_INIT(house5_y_ondemand_mutex);
MUTEX_INIT(house5_y_dyn_light_mutex);
static node_t          house5_y_shared_third_idx[HOUSE5_Y_WINDOW]; /* per-window buf0(C) cursor */
static volatile node_t house5_y_shared_light_idx[2];               /* dynamic light-root cursor */

// On-demand heavy variant: tasklets cooperatively pull C-candidate (buf0) indices from a per-second_root shared cursor.
static ans_t __imp_house5_y_2_coop_window(sysname_t tasklet_id, node_t root, node_t second_root,
                                          int window_idx) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];
    edge_ptr root_begin = row_ptr[root];          // intended DMA
    edge_ptr root_end = row_ptr[root + 1];        // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];   // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1]; // intended DMA
    ans_t ans = 0;

    node_t buf0_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        &col_idx[root_begin], root_end - root_begin,
        mram_buf[tasklet_id], INVALID_NODE);
    if (buf0_size < 3) return 0;   // need at least C, D, E

    while (1) {
        node_t j;
        mutex_lock(house5_y_ondemand_mutex);
        j = house5_y_shared_third_idx[window_idx];
        if (j < buf0_size) house5_y_shared_third_idx[window_idx] = j + 1;
        mutex_unlock(house5_y_ondemand_mutex);
        if (j >= buf0_size) break;

        node_t third_root = mram_buf[tasklet_id][j];   // node C
        edge_ptr third_root_begin = row_ptr[third_root];   // intended DMA
        edge_ptr third_root_end = row_ptr[third_root + 1]; // intended DMA

        node_t buf1_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], buf0_size,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

        for (node_t k = 0; k < buf1_size; k++) {
            node_t fourth_root = mram_buf[tasklet_id + NR_TASKLETS][k];   // node D
            if (fourth_root >= third_root) break;   // D < C (buf1 sorted); breaks C<->D
            ans += (buf0_size - 2);   // E = any other common neighbour (deg-2 vertex)
        }
    }
    return ans;
}
#endif

extern void house5_y(sysname_t tasklet_id) {
    static ans_t partial_ans[NR_TASKLETS];
    static uint64_t partial_cycle[NR_TASKLETS];
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;
    node_t i = 0;

    // heavy roots: tasklets collaborate on the branches of one root
    while (i < root_num) {
        node_t root = roots[i];               // intended DMA
        node_t root_begin = row_ptr[root];    // intended DMA
        node_t root_end = row_ptr[root + 1];  // intended DMA
        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }

        barrier_wait(&co_barrier);
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        partial_ans[tasklet_id] = 0;
#if !defined(HOUSE5_Y_DYNAMIC)
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t second_root = col_idx[j];  // intended DMA
            if (second_root >= root) break;
            partial_ans[tasklet_id] += __imp_house5_y_2(tasklet_id, root, second_root, tasklet_id, NR_TASKLETS);
        }
#else
        // Dynamic: process second_roots in HOUSE5_Y_WINDOW-sized windows sharing per-second_root buf0 cursors, barriers keep windows lock-step.
        {
            int finished = 0;
            for (edge_ptr chunk_start = root_begin; chunk_start < root_end; chunk_start += HOUSE5_Y_WINDOW) {
                if (finished) break;
                barrier_wait(&co_barrier);
                if (tasklet_id == 0)
                    for (int k = 0; k < HOUSE5_Y_WINDOW; k++) house5_y_shared_third_idx[k] = 0;
                barrier_wait(&co_barrier);
                for (int k = 0; k < HOUSE5_Y_WINDOW && (chunk_start + (edge_ptr)k) < root_end; k++) {
                    node_t second_root = col_idx[chunk_start + k];  // intended DMA
                    if (second_root >= root) { finished = 1; break; }
                    partial_ans[tasklet_id] += __imp_house5_y_2_coop_window(tasklet_id, root, second_root, k);
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
            uint64_t total_cycle = 0;
#endif
            for (uint32_t j = 0; j < NR_TASKLETS; j++) {
                total_ans += partial_ans[j];
#ifdef PERF
                total_cycle += partial_cycle[j];
#endif
            }
            ANS_WRITE(i, total_ans);  // intended DMA
#ifdef PERF
            cycle_ct[cur_offset + i] = total_cycle;  // intended DMA
#endif
        }
        i++;
    }

    // light roots: one tasklet per root
#ifdef HOUSE5_Y_DYNAMIC
    // Dynamic light tail: light roots (deg<BRANCH) pulled whole per tasklet via a mutex.
    barrier_wait(&co_barrier);
    if (tasklet_id == 0) house5_y_shared_light_idx[0] = i;
    barrier_wait(&co_barrier);
    while (1) {
        node_t current_i;
        mutex_lock(house5_y_dyn_light_mutex);
            current_i = house5_y_shared_light_idx[0];
            house5_y_shared_light_idx[0]++;
        mutex_unlock(house5_y_dyn_light_mutex);
        if (current_i >= root_num) break;
        node_t root = roots[current_i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(current_i, __imp_house5_y(tasklet_id, root));  // intended DMA
#ifdef PERF
        cycle_ct[cur_offset + current_i] = timer_stop(&cycles[tasklet_id]);  // intended DMA
#endif
    }
#else
    for (i += tasklet_id; i < root_num; i += NR_TASKLETS) {
        node_t root = roots[i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(i, __imp_house5_y(tasklet_id, root));  // intended DMA
#ifdef PERF
        cycle_ct[cur_offset + i] = timer_stop(&cycles[tasklet_id]);  // intended DMA
#endif
    }
#endif
    ans_flush(tasklet_id);
}
