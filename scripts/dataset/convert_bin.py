import os
import numpy as np

TEXT_DIR = "./text"
BIN_DIR  = "./bin"
os.makedirs(BIN_DIR, exist_ok=True)

OUTPUT_NAME_MAP = {
    "com-lj.ungraph": "com-LiveJournal",
}

def parse_meta(f):
    """
    Find the 'Nodes:' and 'Edges:' values in the header comments.
    (e.g. '# Nodes: 123 Edges: 456')
    """
    while True:
        line = f.readline()
        if not line:  # EOF
            raise RuntimeError("No metadata line with 'Nodes:' and 'Edges:' found.")
        if line.startswith("#") and "Nodes:" in line and "Edges:" in line:
            parts = line.split()
            # e.g. ['#','Nodes:','123','Edges:','456']
            return int(parts[2]), int(parts[4])
        # skip any other comment lines

for fname in os.listdir(TEXT_DIR):
    if not fname.endswith(".txt"):
        continue

    in_path = os.path.join(TEXT_DIR, fname)
    stem = os.path.splitext(fname)[0]
    out_name = OUTPUT_NAME_MAP.get(stem, stem)
    out_path = os.path.join(BIN_DIR, out_name + ".bin")
    print(f"[convert] {in_path} -> {out_path}")

    undirected_edges = set()  # canonical edge tuples (min(u,v), max(u,v))
    node_set = set()          # node IDs remaining after dropping self-loops

    with open(in_path, "r") as f:
        meta_nodes, meta_edges = parse_meta(f)  # 1) read metadata

        # 2) read the actual edges
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            try:
                src, dst = map(int, line.split())
                if src == dst:
                    continue  # simple graph: drop self-loops
                if src > dst:
                    src, dst = dst, src
                undirected_edges.add((src, dst))
                node_set.update((src, dst))
            except ValueError:
                print(f"[convert] skip malformed line: {line.strip()}")

    # 3) build the ID mapping (original ID -> 0..N-1)
    id2idx = {nid: i for i, nid in enumerate(sorted(node_set))}
    num_nodes = len(id2idx)

    # Store the undirected simple graph as a directed CSR, per the paper's setup.
    edges = []
    for src, dst in sorted(undirected_edges):
        mapped_src = id2idx[src]
        mapped_dst = id2idx[dst]
        edges.append((mapped_src, mapped_dst))
        edges.append((mapped_dst, mapped_src))
    num_edges = len(edges)

    print(f"   meta  : Nodes {meta_nodes}, Edges {meta_edges}")
    print(f"   actual: Nodes {num_nodes}, Undirected edges {len(undirected_edges)}, Directed edges {num_edges}")

    # 4) adjacency lists -> CSR (row_ptr, col_idx)
    adj = [[] for _ in range(num_nodes)]
    for s, d in edges:
        adj[s].append(d)

    for i in range(num_nodes):
        adj[i].sort()

    row_ptr = np.zeros(num_nodes + 1, dtype=np.int32)
    col_idx = []
    cursor  = 0
    for i in range(num_nodes):
        row_ptr[i] = cursor
        col_idx.extend(adj[i])
        cursor += len(adj[i])
    row_ptr[num_nodes] = cursor  # final pointer

    # 5) write (PimPam format: n, m, row_ptr[0..n-1], col_idx[0..m-1])
    with open(out_path, "wb") as bf:
        bf.write(np.array([num_nodes], dtype=np.int32).tobytes())
        bf.write(np.array([num_edges], dtype=np.int32).tobytes())
        bf.write(row_ptr[:-1].astype(np.int32).tobytes())   # first n entries only
        bf.write(np.array(col_idx, dtype=np.int32).tobytes())

    print(f"[convert] saved {num_nodes} nodes, {num_edges} directed edges")

print("[convert] all conversions done")
