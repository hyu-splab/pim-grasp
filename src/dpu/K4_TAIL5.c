#include <dpu_mine.h>

static ans_t __imp_k4_tail5_2(sysname_t tasklet_id, node_t root, node_t second_root, node_t start, node_t step) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];
    edge_ptr second_root_begin = row_ptr[second_root];
    edge_ptr second_root_end = row_ptr[second_root + 1];
    ans_t ans = 0;

    // threshold = INVALID_NODE: keep all D candidates uncut
    node_t buf0_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[root_begin], root_end - root_begin,
        &col_idx[second_root_begin], second_root_end - second_root_begin, 
        mram_buf[tasklet_id], INVALID_NODE);

    for (node_t j = start; j < buf0_size; j += step) {
        node_t third_root = mram_buf[tasklet_id][j];

        // early exit on C < B (buf0 sorted ascending)
        if (third_root >= second_root) break;

        edge_ptr third_root_begin = row_ptr[third_root];
        edge_ptr third_root_end = row_ptr[third_root + 1];

        node_t buf1_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], buf0_size, 
            &col_idx[third_root_begin], third_root_end - third_root_begin, 
            mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

        for (node_t k = 0; k < buf1_size; k++) {
            node_t fourth_root = mram_buf[tasklet_id + NR_TASKLETS][k]; 
            edge_ptr fourth_root_begin = row_ptr[fourth_root];
            edge_ptr fourth_root_end = row_ptr[fourth_root + 1];
            
            node_t d_degree = fourth_root_end - fourth_root_begin;
            
            if (d_degree > 3) {
                ans += (d_degree - 3);
            }
        }
    }
    return ans;
}

// main function for root-level parallelism
static ans_t __imp_k4_tail5(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];
    edge_ptr root_end = row_ptr[root + 1];
    ans_t ans = 0;
    
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i]; // second K4 vertex (B)

        // symmetry-breaking early exit (B < A)
        if (second_root >= root) break;

        ans += __imp_k4_tail5_2(tasklet_id, root, second_root, 0, 1);
    }
    return ans;
}

#if defined(K4_TAIL5_DYNAMIC)
#include <mutex.h>
#define K4_WINDOW NR_TASKLETS
MUTEX_INIT(k4_ondemand_mutex);
MUTEX_INIT(k4_dyn_light_mutex);
static node_t k4_shared_third_idx[K4_WINDOW];   /* per-window third-root cursor over buf0 */
static volatile node_t k4_shared_light_idx[2];  /* dynamic light-root cursor */

// On-demand heavy variant: tasklets cooperatively pull third-root (buf0) indices from a per-second_root shared cursor.
static ans_t __imp_k4_tail5_2_coop_window(sysname_t tasklet_id, node_t root, node_t second_root,
                                          int window_idx) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];
    edge_ptr root_begin = row_ptr[root];
    edge_ptr root_end = row_ptr[root + 1];
    edge_ptr second_root_begin = row_ptr[second_root];
    edge_ptr second_root_end = row_ptr[second_root + 1];
    ans_t ans = 0;
    node_t buf0_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[root_begin], root_end - root_begin,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        mram_buf[tasklet_id], INVALID_NODE);
    while (1) {
        node_t j;
        mutex_lock(k4_ondemand_mutex);
        j = k4_shared_third_idx[window_idx];
        if (j < buf0_size) k4_shared_third_idx[window_idx] = j + 1;
        mutex_unlock(k4_ondemand_mutex);
        if (j >= buf0_size) break;
        node_t third_root = mram_buf[tasklet_id][j];   // buf0[j], identical across tasklets
        if (third_root >= second_root) {               // buf0 sorted: no more C < B -> stop slot
            mutex_lock(k4_ondemand_mutex);
            k4_shared_third_idx[window_idx] = buf0_size;
            mutex_unlock(k4_ondemand_mutex);
            break;
        }
        edge_ptr third_root_begin = row_ptr[third_root];
        edge_ptr third_root_end = row_ptr[third_root + 1];
        node_t buf1_size = intersect_seq_buf_thresh(tasklet_buf,
            mram_buf[tasklet_id], buf0_size,
            &col_idx[third_root_begin], third_root_end - third_root_begin,
            mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);
        for (node_t k = 0; k < buf1_size; k++) {
            node_t fourth_root = mram_buf[tasklet_id + NR_TASKLETS][k];
            node_t d_degree = row_ptr[fourth_root + 1] - row_ptr[fourth_root];
            if (d_degree > 3) ans += (d_degree - 3);
        }
    }
    return ans;
}
#endif

// DPU entry-point wrapper
extern void k4_tail5(sysname_t tasklet_id) {
    static ans_t partial_ans[NR_TASKLETS];
    static uint64_t partial_cycle[NR_TASKLETS];
    static uint64_t light_partial[NR_TASKLETS];   /* async light-root cycles per tasklet (WALL=max) */
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;   /* OFIS batched collect: write to ans[cur_offset + i] */

    node_t i = 0;
    
    // phase 1: branch-level parallelism for heavy hub roots
    while (i < root_num) {
        node_t root = roots[i];
        node_t root_begin = row_ptr[root];
        node_t root_end = row_ptr[root + 1];

        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }

        barrier_wait(&co_barrier);
        partial_ans[tasklet_id] = 0;
#ifdef PERF
        partial_cycle[tasklet_id] = 0;   /* matching cycles only (barriers excluded) */
#endif

#if !defined(K4_TAIL5_DYNAMIC)
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t second_root = col_idx[j];
            if (second_root >= root) break;

            partial_ans[tasklet_id] += __imp_k4_tail5_2(tasklet_id, root, second_root, tasklet_id, NR_TASKLETS);
        }
#ifdef PERF
        partial_cycle[tasklet_id] = timer_stop(&cycles[tasklet_id]);
#endif
#else
        // Dynamic: process second_roots in K4_WINDOW-sized windows sharing per-second_root third-root cursors, barriers keep windows lock-step.
        {
            int finished = 0;
            for (edge_ptr chunk_start = root_begin; chunk_start < root_end; chunk_start += K4_WINDOW) {
                if (finished) break;
                barrier_wait(&co_barrier);
                if (tasklet_id == 0)
                    for (int k = 0; k < K4_WINDOW; k++) k4_shared_third_idx[k] = 0;
                barrier_wait(&co_barrier);
                for (int k = 0; k < K4_WINDOW && (chunk_start + (edge_ptr)k) < root_end; k++) {
                    node_t second_root = col_idx[chunk_start + k];
                    if (second_root >= root) { finished = 1; break; }
#ifdef PERF
                    timer_start(&cycles[tasklet_id]);
#endif
                    partial_ans[tasklet_id] += __imp_k4_tail5_2_coop_window(tasklet_id, root, second_root, k);
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
            // per-root load: WALL=slowest tasklet (default) or SUM=total work (LB_LOAD_SUM)
            uint64_t load_cycle = 0;
#endif
            for (uint32_t j = 0; j < NR_TASKLETS; j++) {
                total_ans += partial_ans[j];
#ifdef PERF
#ifdef LB_LOAD_SUM
                load_cycle += partial_cycle[j];
#else
                if (partial_cycle[j] > load_cycle) load_cycle = partial_cycle[j];
#endif
#endif
            }
            ANS_WRITE(i, total_ans);
#ifdef PERF
            cycle_ct[cur_offset + i] = load_cycle;  // intended DMA
#endif
        }
        i++;
    }

    // phase 2: independent root-level parallelism for light roots
#if defined(K4_TAIL5_DYNAMIC) && !defined(K4_TAIL5_STATIC_LIGHT)
    // Dynamic light tail: light roots (deg<BRANCH) pulled whole per tasklet via a mutex; -DK4_TAIL5_STATIC_LIGHT reverts to the static tail below.
    barrier_wait(&co_barrier);
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
    if (tasklet_id == 0) k4_shared_light_idx[0] = i;
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    barrier_wait(&co_barrier);
    while (1) {
        node_t current_i;
        mutex_lock(k4_dyn_light_mutex);
            current_i = k4_shared_light_idx[0];
            k4_shared_light_idx[0]++;
        mutex_unlock(k4_dyn_light_mutex);
        if (current_i >= root_num) break;
        node_t root = roots[current_i];
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(current_i, __imp_k4_tail5(tasklet_id, root));
#ifdef PERF
        light_partial[tasklet_id] += timer_stop(&cycles[tasklet_id]);  // per-tasklet accum
        cycle_ct[cur_offset + current_i] = 0;   /* async: WALL is per-tasklet max, set below */
#endif
    }
#ifdef PERF
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {
        uint64_t light_load = 0;   /* async light phase: WALL=slowest tasklet (default) or SUM=total */
        for (uint32_t j = 0; j < NR_TASKLETS; j++)
#ifdef LB_LOAD_SUM
            light_load += light_partial[j];
#else
            if (light_partial[j] > light_load) light_load = light_partial[j];
#endif
        cycle_ct[cur_offset + light_start] = light_load;   /* place light load in one slot */
    }
#endif
#else
    node_t light_start = i;                 /* first light-root index (same across tasklets) */
#ifdef PERF
    light_partial[tasklet_id] = 0;
#endif
    for (i += tasklet_id; i < root_num; i += NR_TASKLETS) {
        node_t root = roots[i];
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(i, __imp_k4_tail5(tasklet_id, root));
#ifdef PERF
        light_partial[tasklet_id] += timer_stop(&cycles[tasklet_id]);  // per-tasklet accum
        cycle_ct[cur_offset + i] = 0;   /* async: WALL is per-tasklet max, set below */
#endif
    }
#ifdef PERF
    barrier_wait(&co_barrier);
    if (tasklet_id == 0 && light_start < root_num) {
        uint64_t light_load = 0;   /* async light phase: WALL=slowest tasklet (default) or SUM=total */
        for (uint32_t j = 0; j < NR_TASKLETS; j++)
#ifdef LB_LOAD_SUM
            light_load += light_partial[j];
#else
            if (light_partial[j] > light_load) light_load = light_partial[j];
#endif
        cycle_ct[cur_offset + light_start] = light_load;   /* place light load in one slot */
    }
#endif
#endif
    ans_flush(tasklet_id);
}
