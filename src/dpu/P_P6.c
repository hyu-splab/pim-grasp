
#define DYNAMIC_MODE 0
#define STATIC_MODE 1

#define P_P6_MACTHING_MODE DYNAMIC_MODE

#if !defined(P_P6_DYNAMIC)

#include <dpu_mine.h>
static ans_t __imp_p_p6_2(sysname_t tasklet_id, node_t root, node_t second_root, node_t start, node_t step) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans = 0;

    // bottom candidates: common neighbors of root and second_root
    node_t bottom_root_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[root_begin], root_end - root_begin,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

    // linear-scan cursor for third_root check
    node_t cur_cmp = 0;

    // pattern needs at least 2 bottom nodes (early prune)
    if (bottom_root_size >= 2) {
        // branch-level parallelism via start/step
        for (edge_ptr j = root_begin + start; j < root_end; j += step) {
            node_t third_root = col_idx[j];  // intended DMA
            if (third_root == second_root) continue;

            edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
            edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA

            // top candidates: common neighbors of second_root and third_root
            node_t fourth_root_size = intersect_seq_buf_thresh(tasklet_buf,
                &col_idx[second_root_begin], second_root_end - second_root_begin,
                &col_idx[third_root_begin], third_root_end - third_root_begin,
                mram_buf[tasklet_id], INVALID_NODE);

            if (!fourth_root_size) continue;

            // intersection of bottom and fourth candidates (IEP overlap count)
            node_t common_size = intersect_seq_buf_thresh(tasklet_buf,
                mram_buf[tasklet_id + NR_TASKLETS], bottom_root_size, 
                mram_buf[tasklet_id], fourth_root_size, 
                mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);

            node_t n1 = bottom_root_size;
            node_t n2 = fourth_root_size;

            // exclude third_root if it is a bottom candidate;
            // sorted col_idx + monotone third_root keeps this O(N) via cur_cmp
            while (cur_cmp < bottom_root_size) {
                node_t bottom_root = mram_buf[tasklet_id + NR_TASKLETS][cur_cmp]; // intended DMA
                if (bottom_root > third_root) {
                    break;
                } else if (bottom_root == third_root) {
                    n1--;
                    cur_cmp++;
                    break;
                }
                cur_cmp++;
            }

            // exclude root if it is a top candidate (binary search in fourth candidates)
            int low = 0, high = fourth_root_size - 1;
            while (low <= high) {
                int mid = low + (high - low) / 2;
                node_t val = mram_buf[tasklet_id][mid]; // intended DMA
                if (val == root) {
                    n2--;
                    break;
                } else if (val < root) {
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            }

            // IEP O(1)
            if (n1 >= 2 && n2 >= 1) {
                ans_t total_combinations = ((ans_t)n1 * (n1 - 1) / 2) * n2;
                ans_t overlap = (ans_t)common_size * (n1 - 1);
                ans += (total_combinations - overlap);
            }
        }
    }
    return ans;
}

static ans_t __imp_p_p6(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    ans_t ans = 0;
    
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i];  // intended DMA
        
        // symmetry breaking
        if (second_root >= root) break;
        
        ans += __imp_p_p6_2(tasklet_id, root, second_root, 0, 1);
    }
    return ans;
}

extern void p_p6(sysname_t tasklet_id) {
    static ans_t partial_ans[NR_TASKLETS];
    static uint64_t partial_cycle[NR_TASKLETS];
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;   /* OFIS batched collect: write to ans[cur_offset + i] */

    node_t i = 0;
    
    // branch-level parallelism for heavy roots
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
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t second_root = col_idx[j];  // intended DMA
            if (second_root >= root) break;
            
            // NR_TASKLETS tasklets split the loop and run concurrently
            partial_ans[tasklet_id] += __imp_p_p6_2(tasklet_id, root, second_root, tasklet_id, NR_TASKLETS);
        }
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

    // root-level parallelism for light roots
    for (i += tasklet_id; i < root_num; i += NR_TASKLETS) {
        node_t root = roots[i];  // intended DMA
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(i, __imp_p_p6(tasklet_id, root));  // intended DMA
#ifdef PERF
        cycle_ct[cur_offset + i] = timer_stop(&cycles[tasklet_id]);  // intended DMA
#endif
    }
    ans_flush(tasklet_id);
}

#else  /* P_P6_DYNAMIC: on-demand (third-root coop-window + light whole-root pull) */

#include <dpu_mine.h>
#include <mutex.h>

MUTEX_INIT(get_next_node_mutex);
MUTEX_INIT(get_next_node_mutex_2);

// window of 16 pre-opened tasks
#define WINDOW_SIZE 16

// third_idx shared by tasklets across 16 second_roots
static volatile node_t shared_third_idx[WINDOW_SIZE];

// shared index for dynamic small-root allocation (8-byte aligned)
static volatile node_t shared_small_root_idx[2] = {};

// single-tasklet standalone (__imp_p_p6_2_single)
static ans_t __imp_p_p6_2_single(sysname_t tasklet_id, node_t root, node_t second_root) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans_ = 0;

    node_t bottom_root_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[root_begin], root_end - root_begin,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

    // linear-scan cursor (kept across the loop)
    node_t cur_cmp = 0;

    if (bottom_root_size >= 2) {
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t third_root = col_idx[j];  // intended DMA
            if (third_root == second_root) continue;

            edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
            edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA

            node_t fourth_root_size = intersect_seq_buf_thresh(tasklet_buf, 
                &col_idx[second_root_begin], second_root_end - second_root_begin, 
                &col_idx[third_root_begin], third_root_end - third_root_begin, 
                mram_buf[tasklet_id], INVALID_NODE);

            if (!fourth_root_size) continue;

            node_t common_size = intersect_seq_buf_thresh(tasklet_buf, 
                mram_buf[tasklet_id + NR_TASKLETS], bottom_root_size, 
                mram_buf[tasklet_id], fourth_root_size, 
                mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);

            node_t n1 = bottom_root_size;
            node_t n2 = fourth_root_size;

            while (cur_cmp < bottom_root_size) {
                node_t bottom_root = mram_buf[tasklet_id + NR_TASKLETS][cur_cmp]; // intended DMA
                if (bottom_root > third_root) {
                    break;
                } else if (bottom_root == third_root) {
                    n1--;
                    cur_cmp++;
                    break;
                }
                cur_cmp++;
            }

            int low = 0, high = fourth_root_size - 1;
            while (low <= high) {
                int mid = low + (high - low) / 2;
                node_t val = mram_buf[tasklet_id][mid]; // intended DMA
                if (val == root) {
                    n2--;
                    break;
                } else if (val < root) {
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            }

            if (n1 >= 2 && n2 >= 1) {
                ans_t total_combinations = ((ans_t)n1 * (n1 - 1) / 2) * n2;
                ans_t overlap = (ans_t)common_size * (n1 - 1);
                ans_ += (total_combinations - overlap);
            }
        }
    }
    return ans_;
}

// single-tasklet standalone main loop (__imp_p_p6)
static ans_t __imp_p_p6(sysname_t tasklet_id, node_t root) {
    edge_ptr root_begin = row_ptr[root];  // intended DMA
    edge_ptr root_end = row_ptr[root + 1];  // intended DMA
    ans_t ans_ = 0;
    
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = col_idx[i];  // intended DMA
        if (second_root >= root) break;
        ans_ += __imp_p_p6_2_single(tasklet_id, root, second_root);
    }
    return ans_;
}

// multi-tasklet cooperative dynamic allocation (__imp_p_p6_2_coop_window)
static ans_t __imp_p_p6_2_coop_window(sysname_t tasklet_id, node_t root, node_t second_root, edge_ptr root_begin, edge_ptr root_end, int window_idx) {
    node_t(*tasklet_buf)[BUF_SIZE] = buf[tasklet_id];

    edge_ptr second_root_begin = row_ptr[second_root];  // intended DMA
    edge_ptr second_root_end = row_ptr[second_root + 1];  // intended DMA
    ans_t ans_ = 0;

    node_t bottom_root_size = intersect_seq_buf_thresh(tasklet_buf,
        &col_idx[root_begin], root_end - root_begin,
        &col_idx[second_root_begin], second_root_end - second_root_begin,
        mram_buf[tasklet_id + NR_TASKLETS], INVALID_NODE);

    // each tasklet scans its own MRAM array, so keep the cur_cmp optimization here too
    node_t cur_cmp = 0;

    if (bottom_root_size >= 2) {
        while (1) {
            edge_ptr j;
            // tasklets grab the shared index on the fly (work-stealing)
            mutex_lock(get_next_node_mutex);
            j = shared_third_idx[window_idx];
            if (shared_third_idx[window_idx] < root_end) {
                shared_third_idx[window_idx]++;
            }
            mutex_unlock(get_next_node_mutex);

            if (j >= root_end) break;

            node_t third_root = col_idx[j];  // intended DMA
            if (third_root == second_root) continue;

            edge_ptr third_root_begin = row_ptr[third_root];  // intended DMA
            edge_ptr third_root_end = row_ptr[third_root + 1];  // intended DMA

            node_t fourth_root_size = intersect_seq_buf_thresh(tasklet_buf, 
                &col_idx[second_root_begin], second_root_end - second_root_begin, 
                &col_idx[third_root_begin], third_root_end - third_root_begin, 
                mram_buf[tasklet_id], INVALID_NODE);

            if (!fourth_root_size) continue;

            node_t common_size = intersect_seq_buf_thresh(tasklet_buf, 
                mram_buf[tasklet_id + NR_TASKLETS], bottom_root_size, 
                mram_buf[tasklet_id], fourth_root_size, 
                mram_buf[tasklet_id + (NR_TASKLETS << 1)], INVALID_NODE);

            node_t n1 = bottom_root_size;
            node_t n2 = fourth_root_size;

            while (cur_cmp < bottom_root_size) {
                node_t bottom_root = mram_buf[tasklet_id + NR_TASKLETS][cur_cmp]; // intended DMA
                if (bottom_root > third_root) {
                    break;
                } else if (bottom_root == third_root) {
                    n1--;
                    cur_cmp++;
                    break;
                }
                cur_cmp++;
            }

            int low = 0, high = fourth_root_size - 1;
            while (low <= high) {
                int mid = low + (high - low) / 2;
                node_t val = mram_buf[tasklet_id][mid]; // intended DMA
                if (val == root) {
                    n2--;
                    break;
                } else if (val < root) {
                    low = mid + 1;
                } else {
                    high = mid - 1;
                }
            }

            if (n1 >= 2 && n2 >= 1) {
                ans_t total_combinations = ((ans_t)n1 * (n1 - 1) / 2) * n2;
                ans_t overlap = (ans_t)common_size * (n1 - 1);
                ans_ += (total_combinations - overlap);
            }
        }
    }
    return ans_;
}

// main scheduling function (p_p6)
extern void p_p6(sysname_t tasklet_id) {
    static volatile ans_t partial_ans[NR_TASKLETS];
    static uint64_t partial_cycle[NR_TASKLETS];
    static perfcounter_cycles cycles[NR_TASKLETS];
    uint64_t cur_offset = offset;   /* OFIS batched collect: write to ans[cur_offset + i] */

    node_t i = 0;
    
    // heavy roots: multi-tasklet window cooperation
    while (i < root_num) {
        node_t root = roots[i];
        node_t root_begin = row_ptr[root];
        node_t root_end = row_ptr[root + 1];

        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }

        barrier_wait(&co_barrier);
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        partial_ans[tasklet_id] = 0;

        int finished = 0;

        for (edge_ptr chunk_start = root_begin; chunk_start < root_end; chunk_start += WINDOW_SIZE) {
            
            if (finished) break;

            barrier_wait(&co_barrier);
            if (tasklet_id == 0) {
                for (int k = 0; k < WINDOW_SIZE; k++) {
                    shared_third_idx[k] = root_begin;
                }
            }
            barrier_wait(&co_barrier);

            // no barrier, so idle tasklets move to the next task without delay
            for (int k = 0; k < WINDOW_SIZE && (chunk_start + k) < root_end; k++) {
                node_t second_root = col_idx[chunk_start + k];
                
                if (second_root >= root) {
                    finished = 1; 
                    break; 
                }
                
                partial_ans[tasklet_id] += __imp_p_p6_2_coop_window(tasklet_id, root, second_root, root_begin, root_end, k);
            }
        }

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
            ANS_WRITE(i, total_ans);
#ifdef PERF
            cycle_ct[cur_offset + i] = total_cycle;
#endif
        }
        i++;
    }

    barrier_wait(&co_barrier);

    // light roots: dynamic allocation
    if (tasklet_id == 0) {
        shared_small_root_idx[0] = i; 
    }
    barrier_wait(&co_barrier);

    while (1) {
        node_t current_i;
        mutex_lock(get_next_node_mutex_2);
            current_i = shared_small_root_idx[0];
            shared_small_root_idx[0]++;
        mutex_unlock(get_next_node_mutex_2);

        if (current_i >= root_num) {
            break;
        }

        node_t root = roots[current_i];
#ifdef PERF
        timer_start(&cycles[tasklet_id]);
#endif
        ANS_WRITE(current_i, __imp_p_p6(tasklet_id, root));
#ifdef PERF
        cycle_ct[cur_offset + current_i] = timer_stop(&cycles[tasklet_id]);
#endif
    }
    ans_flush(tasklet_id);
}

#endif