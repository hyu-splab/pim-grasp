#!/usr/bin/env bash
# download_dataset.sh -- fetch the SNAP source graphs and convert them to the PIM-grasp .bin
# CSR format expected by the host, into paper_artifacts/dataset/.
#
# Output format (int32 little-endian): n, m(=directed=2*undirected), row_ptr[0..n-1], col_idx[m].
# Undirected simple graph: self-loops dropped, each edge stored both directions, node ids
# relabelled 0..n-1 by sorted original id, each adjacency list sorted ascending. This is exactly
# what the repo converters (scripts/dataset/convert_bin.py, convert_pokec.py) produce; we drive
# those unchanged, naming each staged .txt with the DATA_NAME the host resolves to, so the output
# .bin filename already matches (common.h: DATA_PATH = "./data/bin/<DATA_NAME>.bin").
#
#   ./download_dataset.sh                 # the 4 headline graphs: PT CJ LJ PK
#   GRAPHS="PT CJ LJ PK YT CA PP WV" ./download_dataset.sh   # also the fig1 small graphs
#   DATA_DIR=/path/to/dataset ./download_dataset.sh          # custom output dir
#
# Requires: curl (or wget), gunzip, python3 (+numpy).
set -euo pipefail

BUNDLE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${DATA_DIR:-$BUNDLE/dataset}"
CONV="$BUNDLE/scripts/dataset"
GRAPHS="${GRAPHS:-PT CJ LJ PK}"
mkdir -p "$DEST"

python3 -c "import numpy" 2>/dev/null || { echo "ERROR: python3 needs numpy" >&2; exit 1; }
fetch(){ # URL OUTFILE
  if command -v curl >/dev/null; then curl -fL --retry 3 -o "$2" "$1";
  elif command -v wget >/dev/null; then wget -O "$2" "$1";
  else echo "ERROR: need curl or wget" >&2; exit 1; fi; }

# short name | DATA_NAME (== .bin stem the host reads) | SNAP url | header? (SNAP '# Nodes: Edges:')
# VERIFIED canonical snap.stanford.edu URLs for PT CJ LJ PK YT CA PP WV.
meta(){ case "$1" in
  PT) echo "cit-Patents|https://snap.stanford.edu/data/cit-Patents.txt.gz|1";;
  CJ) echo "com-LiveJournal|https://snap.stanford.edu/data/bigdata/communities/com-lj.ungraph.txt.gz|1";;
  LJ) echo "soc-LiveJournall|https://snap.stanford.edu/data/soc-LiveJournal1.txt.gz|1";;   # NOTE: host DATA_NAME has the double-l 'soc-LiveJournall'
  PK) echo "soc-Pokec|https://snap.stanford.edu/data/soc-pokec-relationships.txt.gz|0";;
  YT) echo "com-youtube|https://snap.stanford.edu/data/bigdata/communities/com-youtube.ungraph.txt.gz|1";;
  CA) echo "CA-AstroPh|https://snap.stanford.edu/data/ca-AstroPh.txt.gz|1";;
  PP) echo "p2p-Gnutella04|https://snap.stanford.edu/data/p2p-Gnutella04.txt.gz|1";;
  WV) echo "Wiki-Vote|https://snap.stanford.edu/data/wiki-Vote.txt.gz|1";;
  *)  echo "";;
esac; }

for g in $GRAPHS; do
  m="$(meta "$g")"; [ -n "$m" ] || { echo "skip unknown graph $g"; continue; }
  IFS='|' read -r name url hdr <<<"$m"
  out="$DEST/$name.bin"
  if [ -s "$out" ]; then echo "[$g] $name.bin already present -- skip"; ln -sfn "$name.bin" "$DEST/$g.bin"; continue; fi
  echo "==> [$g] $name  <-  $url"
  STAGE="$(mktemp -d)"; mkdir -p "$STAGE/text" "$STAGE/bin"
  fetch "$url" "$STAGE/$name.txt.gz"
  gunzip -c "$STAGE/$name.txt.gz" > "$STAGE/text/$name.txt"
  if [ "$hdr" = "1" ]; then
    # headered SNAP edge list -> repo convert_bin.py (processes every .txt in ./text)
    ( cd "$STAGE" && python3 -I "$CONV/convert_bin.py" )
  else
    # headerless (soc-Pokec) -> repo convert_pokec.py (reads ./text/soc-pokec-relationships.txt)
    mv "$STAGE/text/$name.txt" "$STAGE/text/soc-pokec-relationships.txt"
    ( cd "$STAGE" && python3 -I "$CONV/convert_pokec.py" )
  fi
  src="$STAGE/bin/$name.bin"
  [ -s "$src" ] || { echo "ERROR: converter did not produce $src" >&2; ls -la "$STAGE/bin" >&2; rm -rf "$STAGE"; exit 1; }
  mv "$src" "$out"
  ln -sfn "$name.bin" "$DEST/$g.bin"   # short-name convenience symlink (mirrors repo data/bin/)
  rm -rf "$STAGE"
  echo "    wrote $out  (+ $g.bin -> $name.bin)"
done

echo
echo "datasets in: $DEST"
ls -la "$DEST"/*.bin 2>/dev/null || true
