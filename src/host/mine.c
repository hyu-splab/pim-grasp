#include <common.h>

static __thread node_t buf[32][MRAM_BUF_SIZE];   /* per-thread: the CPU kernels run concurrently under DEGCYCLE_CPU */

extern node_t intersect(node_t *a, node_t a_size, node_t *b, node_t b_size, node_t *c);
extern node_t difference(node_t *a, node_t a_size, node_t *b, node_t b_size, node_t *c);

ans_t clique2(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        if (g->col_idx[i] >= root) break;
        ans++;
    }
    return ans;
}

ans_t clique3(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;
        edge_ptr second_root_begin = g->row_ptr[second_root];
        edge_ptr second_root_end = g->row_ptr[second_root + 1];
        node_t common_size = intersect(&g->col_idx[root_begin], root_end - root_begin, &g->col_idx[second_root_begin], second_root_end - second_root_begin, buf[0]);
        for (node_t j = 0; j < common_size; j++) {
            if (buf[0][j] >= second_root) break;
            ans++;
        }
    }
    return ans;
}

ans_t clique4(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;
        edge_ptr second_root_begin = g->row_ptr[second_root];
        edge_ptr second_root_end = g->row_ptr[second_root + 1];
        node_t common_size = intersect(&g->col_idx[root_begin], root_end - root_begin, &g->col_idx[second_root_begin], second_root_end - second_root_begin, buf[0]);
        for (node_t j = 0; j < common_size; j++) {
            node_t third_root = buf[0][j];
            if (third_root >= second_root) break;
            edge_ptr third_root_begin = g->row_ptr[third_root];
            edge_ptr third_root_end = g->row_ptr[third_root + 1];
            node_t common_size2 = intersect(buf[0], common_size, &g->col_idx[third_root_begin], third_root_end - third_root_begin, buf[1]);
            for (node_t k = 0; k < common_size2; k++) {
                if (buf[1][k] >= third_root) break;
                ans++;
            }
        }
    }
    return ans;
}

ans_t cycle4(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;
        edge_ptr second_root_begin = g->row_ptr[second_root];
        edge_ptr second_root_end = g->row_ptr[second_root + 1];
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t third_root = g->col_idx[j];
            if (third_root >= second_root) break;
            edge_ptr third_root_begin = g->row_ptr[third_root];
            edge_ptr third_root_end = g->row_ptr[third_root + 1];
            node_t common_size = intersect(&g->col_idx[second_root_begin], second_root_end - second_root_begin, &g->col_idx[third_root_begin], third_root_end - third_root_begin, buf[1]);
            for (node_t k = 0; k < common_size; k++) {
                if (buf[1][k] >= root) break;
                ans++;
            }
        }
    }
    return ans;
}

ans_t house5(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;
        
        edge_ptr second_root_begin = g->row_ptr[second_root];
        edge_ptr second_root_end = g->row_ptr[second_root + 1];
        
        // common neighbours of root and second_root
        node_t fifth_root_size = intersect(&g->col_idx[root_begin], root_end - root_begin,
                                           &g->col_idx[second_root_begin], second_root_end - second_root_begin,
                                           buf[0]);
        
        for (edge_ptr j = root_begin; j < root_end; j++) {
            node_t third_root = g->col_idx[j];
            if (third_root == second_root) continue;
            
            edge_ptr third_root_begin = g->row_ptr[third_root];
            edge_ptr third_root_end = g->row_ptr[third_root + 1];
            
            // common neighbours of second_root and third_root
            node_t fourth_root_size = intersect(&g->col_idx[second_root_begin], second_root_end - second_root_begin,
                                                 &g->col_idx[third_root_begin], third_root_end - third_root_begin,
                                                 buf[1]);

            uint32_t cur_fifth = fifth_root_size > 0 ? fifth_root_size : 0;

            // drop third_root from the fifth candidates (guard against underflow)
            for (node_t k = 0; k < fifth_root_size; k++) {
                if (buf[0][k] == third_root) {
                    if (cur_fifth > 0) {
                        cur_fifth--;
                    }
                    break;
                }
            }

            // intersection of the two candidate sets
            node_t common_size = intersect(buf[0], fifth_root_size, buf[1], fourth_root_size, buf[2]);

            if (fourth_root_size == 0) {
                continue;
            }

            if (fourth_root_size == UINT32_MAX) {
                fourth_root_size = 0;
            }

            // overflow guard before the multiply
            uint64_t multiplier = (uint64_t)(fourth_root_size - 1);
            if ((uint64_t)cur_fifth * multiplier > UINT64_MAX) {
                printf("Overflow detected: cur_fifth=%u, fourth_root_size=%u\n", cur_fifth, fourth_root_size);
                ans += UINT64_MAX;
            } else {
                ans += ((ans_t)cur_fifth) * (fourth_root_size - 1);
            }

            ans -= common_size;
        }
    }
    return ans;
}


ans_t tri_tri6(Graph *g, node_t root) {
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;
        edge_ptr second_root_begin = g->row_ptr[second_root];
        edge_ptr second_root_end = g->row_ptr[second_root + 1];
        node_t common_size = intersect(&g->col_idx[root_begin], root_end - root_begin, &g->col_idx[second_root_begin], second_root_end - second_root_begin, buf[0]);
        if(common_size <= 1) continue;
        for (node_t j = 0; j < common_size; j++) {
            node_t third_root = buf[0][j];
            if (third_root >= second_root) break;
            edge_ptr third_root_begin = g->row_ptr[third_root];
            edge_ptr third_root_end = g->row_ptr[third_root + 1];
            node_t common_size2 = intersect(&g->col_idx[second_root_begin], second_root_end - second_root_begin, &g->col_idx[third_root_begin], third_root_end - third_root_begin, buf[1]);
            node_t common_size3 = intersect(&g->col_idx[root_begin], root_end - root_begin, &g->col_idx[third_root_begin], third_root_end - third_root_begin, buf[2]);
            node_t common_size123 = intersect(buf[0], common_size, buf[1], common_size2, buf[3]);
            if(common_size2 <= 1 || common_size3 <= 1 || common_size123 <= 1) continue;
            ans += ((ans_t)MAX(1, common_size) - 1) * (MAX(1, common_size2) - 1) * (MAX(1, common_size3) - 1) - MAX(0, ((ans_t)common_size123) * (((ans_t)common_size) + common_size2 + common_size3 - 5));
        }
    }
    return ans;
}

// Independent brute-force reference (adjacency binary-search, does not use intersect()).
static inline int h5y_adj(Graph *g, node_t u, node_t v) {
    edge_ptr lo = g->row_ptr[u], hi = g->row_ptr[u + 1];
    while (lo < hi) { edge_ptr m = lo + (hi - lo) / 2;
        if (g->col_idx[m] == v) return 1;
        if (g->col_idx[m] < v) lo = m + 1; else hi = m; }
    return 0;
}
ans_t house5_y_bf(Graph *g, node_t A) {
    ans_t cnt = 0;
    edge_ptr ab = g->row_ptr[A], ae = g->row_ptr[A + 1];
    static _Thread_local node_t cn[4096];
    for (edge_ptr i = ab; i < ae; i++) {
        node_t B = g->col_idx[i]; if (B >= A) break;
        node_t nc = 0;
        for (edge_ptr t = ab; t < ae; t++) { node_t x = g->col_idx[t];
            if (x != B && h5y_adj(g, B, x) && nc < 4096) cn[nc++] = x; }  /* common nbrs of A,B */
        if (nc < 3) continue;
        for (node_t a = 0; a < nc; a++) { node_t C = cn[a];
            for (node_t b2 = 0; b2 < nc; b2++) { node_t D = cn[b2];
                if (D >= C) break;
                if (h5y_adj(g, C, D)) cnt += (ans_t)(nc - 2); } }
    }
    return cnt;
}

// HOUSE5_Y (P5): single-thread CPU port of dpu/HOUSE5_Y.c.
ans_t house5_y(Graph *g, node_t root) {
    static int bf = -1;
    if (bf < 0) bf = getenv("H5Y_BF") ? 1 : 0;
    if (bf) return house5_y_bf(g, root);
    edge_ptr root_begin = g->row_ptr[root];
    edge_ptr root_end   = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = root_begin; i < root_end; i++) {
        node_t second_root = g->col_idx[i];
        if (second_root >= root) break;               // B < A
        edge_ptr sr_begin = g->row_ptr[second_root];
        edge_ptr sr_end   = g->row_ptr[second_root + 1];
        node_t buf0_size = intersect(&g->col_idx[root_begin], root_end - root_begin,
                                     &g->col_idx[sr_begin],  sr_end - sr_begin, buf[0]);
        if (buf0_size < 3) continue;                  // need C, D, E
        for (node_t j = 0; j < buf0_size; j++) {
            node_t C = buf[0][j];                     // third_root
            edge_ptr c_begin = g->row_ptr[C];
            edge_ptr c_end   = g->row_ptr[C + 1];
            node_t buf1_size = intersect(buf[0], buf0_size,
                                         &g->col_idx[c_begin], c_end - c_begin, buf[1]);
            for (node_t k = 0; k < buf1_size; k++) {
                node_t D = buf[1][k];                 // fourth_root
                if (D >= C) break;                    // D < C (buf1 sorted)
                ans += (ans_t)(buf0_size - 2);        // E = any other common neighbour
            }
        }
    }
    return ans;
}

// P_P6 (P4): single-thread CPU port of dpu/P_P6.c (IEP-based).
ans_t p_p6(Graph *g, node_t root) {
    edge_ptr rb = g->row_ptr[root], re = g->row_ptr[root + 1];
    ans_t ans = 0;
    for (edge_ptr i = rb; i < re; i++) {
        node_t B = g->col_idx[i];
        if (B >= root) break;                                   // B < A
        edge_ptr sb = g->row_ptr[B], se = g->row_ptr[B + 1];
        node_t n1s = intersect(&g->col_idx[rb], re - rb, &g->col_idx[sb], se - sb, buf[0]);  // bottom
        if (n1s < 2) continue;
        for (edge_ptr j = rb; j < re; j++) {
            node_t C = g->col_idx[j];
            if (C == B) continue;
            edge_ptr cb = g->row_ptr[C], ce = g->row_ptr[C + 1];
            node_t n2s = intersect(&g->col_idx[sb], se - sb, &g->col_idx[cb], ce - cb, buf[1]);  // upper
            if (!n2s) continue;
            node_t common = intersect(buf[0], n1s, buf[1], n2s, buf[2]);
            node_t n1 = n1s, n2 = n2s;
            { int lo = 0, hi = (int)n1s - 1; while (lo <= hi) { int m = lo + (hi - lo) / 2; node_t v = buf[0][m];
                if (v == C) { n1--; break; } else if (v < C) lo = m + 1; else hi = m - 1; } }   // exclude C from bottom
            { int lo = 0, hi = (int)n2s - 1; while (lo <= hi) { int m = lo + (hi - lo) / 2; node_t v = buf[1][m];
                if (v == root) { n2--; break; } else if (v < root) lo = m + 1; else hi = m - 1; } }  // exclude A from upper
            if (n1 >= 2 && n2 >= 1) {
                ans_t total   = ((ans_t)n1 * (n1 - 1) / 2) * n2;
                ans_t overlap = (ans_t)common * (n1 - 1);
                ans += (total - overlap);
            }
        }
    }
    return ans;
}