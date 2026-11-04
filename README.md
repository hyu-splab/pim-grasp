# PIM-grasp (PIM-based graph spotter)

PIM-grasp is a PIM-based graph pattern-matching framework. It relies on an on-demand
task-assignment policy to mitigate the straggler problem, exploiting the on-the-fly host-PIM
interaction scheme (OFIS). It dynamically generates new subgraphs on the CPU while DPUs perform
matching tasks, effectively hiding preprocessing latency, and minimizes communication overhead
using dynamic task batching and batched result transfer.

PIM-grasp uses **libofis**, the OFIS library, which replaces `libdpu.so` in the standard UPMEM
SDK.

## Engines

The paper compares three engines:

| engine | description |
|---|---|
| **PimPam** | load-balanced task assignment with DPU-based subgraph generation, modified to collect results in a single parallel transfer and to iterate when a graph does not fit (baseline) |
| **OFIS-PimPam** | on-demand task assignment with task-unit subgraphs generated on the CPU (baseline) |
| **PIM-grasp** | overlapped subgraph generation, dynamic task batching, multi-granular host-PIM interactions, intra-DPU on-demand task assignment, and batched result collection |

Query patterns: **P1 = HOUSE5, P2 = HOUSE5_X, P3 = K4_TAIL5, P4 = P_P6, P5 = HOUSE5_Y.**
Datasets: **PT = cit-Patents, CJ = com-LiveJournal, LJ = soc-LiveJournal, PK = soc-Pokec.**

## System Requirements

- UPMEM PIM DIMMs (at least 1,024 DPUs)
- Ubuntu 22.04 LTS
- UPMEM SDK ver 2024.2.0 (provided by libofis)
- UPMEM kernel driver ver 2024.2.0 (included in `driver/`)
- libofis (https://github.com/hyu-splab/libofis)
- sudo privileges
- Linux kernel headers
- gcc, GNU make, pkg-config, python3 (numpy, matplotlib), gnuplot, curl or wget, gunzip
- libpython3.10, libelf, libnuma
- a few GB of free disk space for datasets

## Files

```
src/                 source code of the three engines
code/                figure generators
scripts/             experiment drivers, log parsers, figure rendering, and dataset converters
driver/              UPMEM kernel driver source
exp_all.sh           runs all experiments and generates the figures
download_dataset.sh  downloads the datasets
```

## Setup UPMEM SDK

In a directory outside this repository, unpack the SDK provided by libofis:

```bash
git clone https://github.com/hyu-splab/libofis.git
tar xzf libofis/sdk/files/upmem-2024.2.0-Linux-x86_64.tar.gz
export UPMEM_HOME="$PWD/upmem-2024.2.0-Linux-x86_64"
export PATH="$UPMEM_HOME/bin:$PATH"
```

## Install and Setup libofis

```bash
cp libofis/ofis/libofis.so $UPMEM_HOME/lib
cp libofis/ofis/ofis.h     $UPMEM_HOME/include/dpu
( cd $UPMEM_HOME/lib && ln -sfn libofis.so libdpu.so.0.0 && ln -sfn libdpu.so.0.0 libdpu.so )
```

Run the remaining steps from this repository.

## Install UPMEM Kernel Driver

Skip this section if the driver is already installed.

```bash
make -C driver/upmem-driver-2024.2.0
sudo make -C driver/upmem-driver-2024.2.0 modules_install
sudo depmod -a
sudo install -D -m 644 $UPMEM_HOME/share/upmem/include/misc/internalStateReset.bin /lib/firmware/upmem/internalStateReset.bin
sudo modprobe dpu
```

## Download Dataset

```bash
./download_dataset.sh
```

Datasets are placed under `dataset/`.

## Experiments (How to Run)

```bash
./exp_all.sh [num_iter]   # repetitions per data point (default 1)
```

Runs all experiments and generates the figures under `figures_out/`. Raw results are placed under
`result/`, and plot inputs under `data/`. While running, `exp_all.sh` sets
`kernel.numa_balancing=1` and restores the previous value on exit.

To run a part of it:

```bash
EXPERIMENTS="overall overlap" ./exp_all.sh   # only the listed experiments
ONLY_GRAPHS="PT" ./exp_all.sh                # only the listed graphs
PHASES="parse plot" ./exp_all.sh             # regenerate the figures from an existing result/
```

## Experiments and Paper Figures

Pass the names in the `EXPERIMENTS` column to `exp_all.sh` to produce a figure. Files are
written to `figures_out/` as `.eps`, `.pdf`, and `.png`.

| paper figure | content | files in `figures_out/` | `EXPERIMENTS` |
|---|---|---|---|
| Figure 1 | E2E times of PimPam and OFIS-PimPam (CJ, P1-P5) | `breakdown` | `overall` |
| Figure 3 | Correlation between the root vertex's degree and task execution time on the CPU (PT, P1 and P4) | `deg_HOUSE5_PT_cpu`, `deg_P_P6_PT_cpu` | `degcpu` |
| Figure 8 | E2E times for matching P1-P5 | `overall_{PT,CJ,PK,LJ}` | `overall` |
| Figure 9 | Breakdown of the E2E time for matching P1 | `breakdown_overall` | `overall` |
| Figure 10 | Speedup of PIM-grasp against PimPam (PT, LJ) | `dpu_speedup_{PT,LJ}` | `overall` |
| Figure 11 | Breakdown of the E2E times for matching P1 on PT | `breakdown_dpu_PT` | `overall` |
| Figure 12 | Effect of the dynamic task batching on the overall data transfer time (P1) | `batching_nbatch`, `batching_xfer` | `batching` |
| Figure 13 | DPU execution time with and without intra-DPU on-demand task assignments | `tasklet_idle`, `ondemand_mean` | `tasklet_idle ondemand` |
| Figure 14 | Result collection time with and without batched result collection (P1) | `collect_nbatch`, `collect_compare` | `collect` |
| Figure 15 | Effect of overlapping subgraph generation with DPU execution on the E2E time | `overlap_{PT,CJ,PK,LJ}` | `overlap` |

`overall` produces Figures 1, 8, 9, 10, and 11 from one run, and `overlap` also runs `overall`.
Each `exp_all.sh` run replaces `figures_out/`, so list all the experiments you need in one
`EXPERIMENTS` value.

Figures 2, 4, 5, 6, and 7 are diagrams and are not generated by this artifact.
