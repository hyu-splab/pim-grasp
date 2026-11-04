#!/usr/bin/env python3
# One-off converter for soc-Pokec (SNAP raw edge list, no header) into the PimPam
# .bin CSR format, identical to convert_bin.py:
#   int32 n, int32 m(=directed=2*undirected), row_ptr[0..n-1] (int32), col_idx[m] (int32)
# Undirected simple graph: self-loops dropped, each edge stored both directions,
# node ids relabelled 0..n-1 by sorted original id, adjacency sorted ascending.
import numpy as np

IN  = "./text/soc-pokec-relationships.txt"
OUT = "./bin/soc-Pokec.bin"

und = set()
nodes = set()
with open(IN) as f:
    for line in f:
        if not line or line[0] == "#":
            continue
        a, b = line.split()
        u = int(a); v = int(b)
        if u == v:
            continue
        if u > v:
            u, v = v, u
        und.add((u, v))
        nodes.add(u); nodes.add(v)

id2idx = {nid: i for i, nid in enumerate(sorted(nodes))}
n = len(id2idx)
print(f"nodes={n}  undirected_edges={len(und)}")

# directed edge list
deg = np.zeros(n, dtype=np.int64)
for (u, v) in und:
    deg[id2idx[u]] += 1
    deg[id2idx[v]] += 1
m = int(deg.sum())
row_ptr = np.zeros(n + 1, dtype=np.int64)
np.cumsum(deg, out=row_ptr[1:])
col_idx = np.empty(m, dtype=np.int32)
cur = row_ptr[:-1].copy()
for (u, v) in und:
    su, sv = id2idx[u], id2idx[v]
    col_idx[cur[su]] = sv; cur[su] += 1
    col_idx[cur[sv]] = su; cur[sv] += 1
# sort each adjacency list ascending (required by the kernels' binary search / sorted intersect)
for i in range(n):
    s, e = row_ptr[i], row_ptr[i + 1]
    col_idx[s:e] = np.sort(col_idx[s:e])

with open(OUT, "wb") as bf:
    bf.write(np.array([n], dtype=np.int32).tobytes())
    bf.write(np.array([m], dtype=np.int32).tobytes())
    bf.write(row_ptr[:-1].astype(np.int32).tobytes())   # n entries only
    bf.write(col_idx.astype(np.int32).tobytes())
print(f"saved {OUT}  n={n} m(directed)={m}  avg_deg={m/n:.2f}")
