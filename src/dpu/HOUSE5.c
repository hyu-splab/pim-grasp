#include <dpu_mine.h>
#if defined(HOUSE5_ONDEMAND) || defined(HOUSE5_ONDEMAND_NEW) || defined(HOUSE5_DYNAMIC)
#include <mutex.h>
#endif

#ifdef HOUSE5_ONDEMAND_NEW
// Batch-level dynamic work pool: tasklets pull (root, second_root<root) pairs from a shared queue.
MUTEX_INIT(house5_pool_mutex);
static node_t   h5_pool_root_i;   /* current batch-root index in the shared iterator */
static edge_ptr h5_pool_j;        /* current neighbor offset within roots[h5_pool_root_i] */
#ifndef HOUSE5_MAX_BATCH_ROOTS
#define HOUSE5_MAX_BATCH_ROOTS 64 /* must be >= host ROOTS_PER_DPU */
#endif
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

#if defined(HOUSE5_ONDEMAND) || defined(HOUSE5_DYNAMIC)
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

#ifdef HOUSE5_ONDEMAND_NEW
    // batch-level dynamic (root,second_root) pool
    static ans_t partial_ans2[NR_TASKLETS][HOUSE5_MAX_BATCH_ROOTS];
    // each tasklet clears its own row; tasklet 0 arms the shared iterator
    for (node_t r = 0; r < root_num; ++r) partial_ans2[tasklet_id][r] = 0;
    if (tasklet_id == 0) {
        h5_pool_root_i = 0;
        h5_pool_j = (root_num > 0) ? row_ptr[roots[0]] : 0;
    }
    barrier_wait(&co_barrier);

    // pull loop: dynamically grab the next (root, second_root<root) pair
    while (1) {
        node_t ri; edge_ptr jj; int got = 0;
        mutex_lock(house5_pool_mutex);
        while (h5_pool_root_i < root_num) {
            const node_t rt   = roots[h5_pool_root_i];        // intended DMA
            const edge_ptr re = row_ptr[rt + 1];              // intended DMA
            if (h5_pool_j >= re) {                            // this root exhausted
                if (++h5_pool_root_i < root_num) h5_pool_j = row_ptr[roots[h5_pool_root_i]];
                continue;
            }
            const node_t sr = col_idx[h5_pool_j];             // intended DMA
            if (sr >= rt) {                                   // sorted: no more second_root<root
                if (++h5_pool_root_i < root_num) h5_pool_j = row_ptr[roots[h5_pool_root_i]];
                continue;
            }
            ri = h5_pool_root_i; jj = h5_pool_j; ++h5_pool_j; got = 1;
            break;
        }
        mutex_unlock(house5_pool_mutex);
        if (!got) break;
        // one self-contained (root, second_root) contribution (full third-root loop)
        partial_ans2[tasklet_id][ri] += __imp_house5_2(tasklet_id, roots[ri], col_idx[jj], 0, 1);
    }
    barrier_wait(&co_barrier);

    // reduce per root: ans[root] = sum over tasklets of its partials
    for (node_t r = tasklet_id; r < root_num; r += NR_TASKLETS) {
        ans_t s = 0;
        for (uint32_t t = 0; t < NR_TASKLETS; ++t) s += partial_ans2[t][r];
        ANS_WRITE(r, s);                                      // intended DMA
#ifdef PERF
        cycle_ct[cur_offset + r] = 0; /* pooled work: per-root cycle not attributed */
#endif
    }
    ans_flush(tasklet_id);
    return;
#endif

    node_t i = 0;
    while (i < root_num) {
        node_t root = roots[i];  // intended DMA
        node_t root_begin = row_ptr[root];  // intended DMA
        node_t root_end = row_ptr[root + 1];  // intended DMA

#ifndef HOUSE5_ONDEMAND
        // static: heavy roots (deg>=BRANCH) cooperate here; light roots go to the
        // per-tasklet tail loop below, so break at the first light root. On-demand has
        // no tail loop and processes all roots here, so it must not break.
        if (root_end - root_begin < BRANCH_LEVEL_THRESHOLD) {
            break;
        }
#endif

        barrier_wait(&co_barrier);
        partial_ans[tasklet_id] = 0;
#ifdef PERF
        partial_cycle[tasklet_id] = 0;   /* matching cycles only (barriers excluded) */
#endif
#if !defined(HOUSE5_ONDEMAND) && !defined(HOUSE5_DYNAMIC)
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
            // per-root load metric: WALL (default) / SUM (LB_LOAD_SUM) / IDLE (TASKLET_IDLE).
            uint64_t _mx = 0, _sm = 0;
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
            { uint64_t _idle = 0;   /* Σ_j (max - partial_j), per-term to avoid underflow */
              for (uint32_t j = 0; j < NR_TASKLETS; j++)
                  if (partial_cycle[j] < _mx) _idle += _mx - partial_cycle[j];
              cycle_ct[cur_offset + i] = _idle; }  // intended DMA
#elif defined(LB_LOAD_SUM)
            cycle_ct[cur_offset + i] = _sm;  // intended DMA
#else
            cycle_ct[cur_offset + i] = _mx;  // intended DMA
#endif
#endif
        }
        i++;
    }

#if defined(HOUSE5_DYNAMIC) && !defined(HOUSE5_STATIC_LIGHT)
    // Dynamic light tail: light roots (deg<BRANCH) pulled whole per tasklet via a mutex; -DHOUSE5_STATIC_LIGHT reverts to the static tail below.
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
        uint64_t _mx = 0, _sm = 0;   /* light phase: WALL=max, SUM=total, IDLE=Σ(max-each) */
        for (uint32_t j = 0; j < NR_TASKLETS; j++) {
            _sm += light_partial[j];
            if (light_partial[j] > _mx) _mx = light_partial[j];
        }
#ifdef TASKLET_IDLE
        { uint64_t _idle = 0;
          for (uint32_t j = 0; j < NR_TASKLETS; j++)
              if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
          cycle_ct[cur_offset + light_start] = _idle; }
#elif defined(LB_LOAD_SUM)
        cycle_ct[cur_offset + light_start] = _sm;
#else
        cycle_ct[cur_offset + light_start] = _mx;
#endif
    }
#endif
#elif !defined(HOUSE5_ONDEMAND)
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
        uint64_t _mx = 0, _sm = 0;   /* light phase: WALL=max, SUM=total, IDLE=Σ(max-each) */
        for (uint32_t j = 0; j < NR_TASKLETS; j++) {
            _sm += light_partial[j];
            if (light_partial[j] > _mx) _mx = light_partial[j];
        }
#ifdef TASKLET_IDLE
        { uint64_t _idle = 0;
          for (uint32_t j = 0; j < NR_TASKLETS; j++)
              if (light_partial[j] < _mx) _idle += _mx - light_partial[j];
          cycle_ct[cur_offset + light_start] = _idle; }
#elif defined(LB_LOAD_SUM)
        cycle_ct[cur_offset + light_start] = _sm;
#else
        cycle_ct[cur_offset + light_start] = _mx;
#endif
    }
#endif
#endif
    ans_flush(tasklet_id);
}
