# AMD DMA-Latte

AMD DMA-Latte benchmarks DMA-accelerated GPU communication collectives on AMD Instinct GPUs. It implements all-gather and all-to-all using AMD's system-DMA (sDMA) engines — hardware that transfers data across GPU HBM and xGMI links without involving GPU compute units (CUs). The repo sweeps transfer sizes, reports latency and bandwidth, and compares different DMA collective implementation strategies: parallel copy, broadcast, in-place swap, and back-to-back copies. It also includes tools to characterize DMA copy costs and point-to-point command latency breakdown.

This repository accompanies our paper:

> **DMA-Latte: Expanding the Reach of DMA Offloads to Latency-bound ML Communication**  
> Suchita Pati, Shaizeen Aga, Mahzabeen Islam, Ryan Quach, Saleel Kudchadker, Mohamed Assem Ibrahim  
> To appear, IEEE/ACM MICRO 2026 · [[arXiv:2511.06605]](https://arxiv.org/abs/2511.06605)

`DMA-Latte is a play on "latency-bound" region — the regime where DMA has traditionally been avoided for being too slow. Our optimizations show that, with the right DMA features, DMA can be competitive even at small transfer sizes.`

## Table of Contents

- [Getting Started](#getting-started)
  - [Supported Hardware](#supported-hardware)
  - [Dependencies and Build](#dependencies-and-build)
  - [Engine Affinity Map](#engine-affinity-map)
- [Running Experiments](#running-experiments)
  - [Quick Start](#quick-start)
  - [Collective Benchmarks](#collective-benchmarks)
    - [Parameters](#parameters)
    - [All-Gather](#all-gather)
    - [All-to-All](#all-to-all)
  - [DMA Copy Phase Breakdown](#dma-copy-phase-breakdown)
- [Validation](#validation)
- [Citation](#citation)
- [Related Publications](#related-publications)
- [Acknowledgements](#acknowledgements)

## Getting Started

### Supported Hardware

- **MI300X / MI325X / MI355X (gfx942)** — primary target. Optimized for SPX compute partition mode:
  ```bash
  rocm-smi --setcomputepartition=SPX
  ```
  An engine affinity map is recommended for best performance (see [Engine Affinity Map](#engine-affinity-map)).
- **MI210 (gfx90a)** — supported.

### Dependencies and Build

Requires ROCm installed on the system ([https://rocm.docs.amd.com](https://rocm.docs.amd.com)).

```bash
git clone https://github.com/AMDResearch/AMD-DMA-Latte
cd AMD-DMA-Latte

# Default: uses /opt/rocm
make

# Override ROCm path if needed:
make ROCM_PATH=/opt/rocm-7.2.0
```

### Engine Affinity Map

On MI3xx GPUs, sDMA engines have affinity to specific destination GPUs. Providing the correct engine for each (src, dst) GPU pair improves performance.

**Generate the map for your machine:**
```bash
python3 scripts/gen_sdma_map_sysfs.py --out engineMap.csv
```

Without a map, round-robin engine assignment is used (fine for MI210; suboptimal for MI3xx).

## Running Experiments

### Quick Start

```bash
# 8-way all-gather parallel copy (MI300X / MI325X / MI355X)
SDMA_MAP=engineMap.csv bash scripts/run_ag_pcpy.sh

# All-to-all parallel copy
SDMA_MAP=engineMap.csv bash scripts/run_aa_pcpy.sh

# DMA copy phase breakdown
SDMA_MAP=engineMap.csv COLLECTIVE_TYPE=p2p VARIANT=latency ./sdma_collective
```

### Collective Benchmarks

All collective benchmarks sweep sizes from `MIN_SIZE` to `MAX_SIZE` (doubling each step) and print one CSV row per size:

```
size(B),time(us),algbw(GB/s),busbw(GB/s)
```

- `size`: total collective size in bytes (`chunk × N GPUs`)
- `time`: average end-to-end latency in microseconds (first 5 iterations discarded as warmup)
- `algbw`: algorithmic bandwidth — `(N-1)/N × total_bytes / time`
- `busbw`: per-link bandwidth — `total_bytes / (N × time)`

With `SDMA_VERBOSE=1`, also prints per-engine control overhead and per-transfer timing.

#### Parameters

| Variable | Description | Values / Default |
|----------|-------------|-----------------|
| `COLLECTIVE_TYPE` | Collective type | `ag` (all-gather), `aa` (all-to-all), `p2p` (default: `ag`) |
| `VARIANT` | Implementation variant | `pcpy`, `bcst`, `swap`, `b2b` (collectives); `latency` (p2p) |
| `SDMA_MAP` | Engine affinity map | File path (optional; round-robin if omitted) |
| `NUM_GPUS` | Number of GPUs | Integer ≥ 2 (default: 8) |
| `GPU_LIST` | Explicit GPU indices, e.g. `"0,1,4,5"` — overrides `NUM_GPUS` | Comma-separated list |
| `MIN_SIZE` | Minimum collective size in bytes | Integer (default: 128) |
| `MAX_SIZE` | Maximum collective size in bytes | Integer (default: 1GB) |
| `NUM_ITER` | Iterations (first 5 are warmup) | Integer ≥ 6 (default: 20) |
| `SYNC_TYPE` | Completion command | `0`: 32b fence  `1`: 2×32b fence  `2`: 64b atomic (default: 2) |
| `PRELAUNCH` | Off-critical-path scheduling: queue all commands before triggering | `0`/`1` (default: 0) |
| `MULTI_SIGNAL` | Each engine uses a unique trigger signal — all engines wait on their own signal before starting (used with `PRELAUNCH=1`) | `0`/`1` (default: 0) |
| `B2B_DMA` | Comma-separated xGMI engine IDs for `b2b` variants (e.g. `"8,10"`) | `8` |
| `B2B_WAYS` | Destinations per engine for `b2b` variants — higher uses fewer engines with more back-to-back work per engine | `7` |
| `SDMA_VERBOSE` | Print engine assignments, per-packet details, and timing breakdown | Any value (default: off) |

**`SYNC_TYPE` details:**
- `0`: SDMA fence packet, 32-bit write to signal memory.
- `1`: Two 32-bit fence writes (HSA compatible).
- `2`: SDMA 64-bit atomic decrement (HSA compatible).

**`PRELAUNCH=1`:** Schedules all DMA commands off the critical path — commands are queued ahead of time and triggered with a memory write, removing command-creation overhead from the latency-critical window.

#### All-Gather

| `VARIANT` | Description | Script |
|-----------|-------------|--------|
| `pcpy` | Parallel copy — one sDMA engine per destination GPU | `run_ag_pcpy.sh` |
| `bcst` | Broadcast packet — single command writes to 2 destinations | `run_ag_bcst.sh` |
| `b2b` | Back-to-back copies — all destination copies from a GPU are issued on one engine, reducing synchronization overhead (number of engines and destinations per engine configurable via `B2B_DMA`/`B2B_WAYS`) | `run_ag_b2b.sh` |

#### All-to-All

| `VARIANT` | Description | Script |
|-----------|-------------|--------|
| `pcpy` | Parallel copy | `run_aa_pcpy.sh` |
| `swap` | In-place swap — bidirectional transfer in a single DMA command | `run_aa_swap.sh` |
| `b2b` | Back-to-back copies — all destination copies from a GPU are issued on one engine, reducing synchronization overhead (number of engines and destinations per engine configurable via `B2B_DMA`/`B2B_WAYS`) | `run_aa_b2b.sh` |

These variants correspond directly to the DMA features analyzed in the DMA-Latte paper. `pcpy` is the baseline; `bcst`, `swap`, and `b2b` reduce DMA command count and synchronization overhead.

For speedup of each variant relative to the `pcpy` baseline, see [PERFORMANCE.md](PERFORMANCE.md).

### DMA Copy Phase Breakdown

```bash
SDMA_MAP=engineMap.csv COLLECTIVE_TYPE=p2p VARIANT=latency ./sdma_collective
```

Measures DMA command latency phases for a single GPU-to-GPU copy. Isolates four phases:

- **control** — CPU command creation and ring buffer submission
- **schedule** — CPU-to-DMA handover via doorbell (GPU timestamp)
- **copy** — DMA transfer (GPU timestamp)
- **sync** — completion signal propagation (GPU timestamp)

Non-copy phases dominate at small transfer sizes. See [PERFORMANCE.md](PERFORMANCE.md) for measured phase breakdowns and Section 3 of the DMA-Latte paper.

Default output: `size,control,schedule,copy,sync`. Use `SDMA_VERBOSE=1` for further sub-phase breakdowns.

| Parameter | Description | Default |
|-----------|-------------|---------|
| `SRC_GPU` | Source GPU index | 0 |
| `DST_GPU` | Destination GPU index | 1 |


## Validation

Each collective run validates correctness after timing. GPU buffers are pre-filled with deterministic patterns and results are compared element-by-element on the CPU.

## Citation

If you use AMD DMA-Latte in your research, please cite:

> Suchita Pati, Shaizeen Aga, Mahzabeen Islam, Ryan Quach, Saleel Kudchadker, Mohamed Assem Ibrahim. "DMA-Latte: Expanding the Reach of DMA Offloads to Latency-bound ML Communication." To appear, IEEE/ACM MICRO 2026. arXiv:2511.06605.

```
@misc{pati2026dmalatteexpandingreachdma,
  title={DMA-Latte: Expanding the Reach of DMA Offloads to Latency-bound
         ML Communication},
  author={Suchita Pati and Shaizeen Aga and Mahzabeen Islam and
          Ryan Quach and Saleel Kudchadker and Mohamed Assem Ibrahim},
  year={2026},
  eprint={2511.06605},
  archivePrefix={arXiv},
  primaryClass={cs.DC},
  url={https://arxiv.org/abs/2511.06605}
}
```

```
@software{AMDDMALatte,
  title={AMD DMA-Latte},
  author={Suchita Pati and Shaizeen Aga and Mahzabeen Islam and
          Ryan Quach and Saleel Kudchadker and Mohamed Assem Ibrahim},
  year={2026},
  publisher={AMD},
  url={https://github.com/AMDResearch/AMD-DMA-Latte}
}
```

## Related Publications

1. [[arXiv:2511.06605]](https://arxiv.org/abs/2511.06605) Pati et al. "DMA-Latte: Expanding the Reach of DMA Offloads to Latency-bound ML Communication." To appear, IEEE/ACM MICRO 2026.
2. [[arXiv:2512.10236]](https://arxiv.org/abs/2512.10236) Pal et al. "Design Space Exploration of DMA based Finer-Grain Compute Communication Overlap." To appear, IISWC 2026.
3. [[arXiv:2412.14335]](https://arxiv.org/abs/2412.14335) Agrawal et al. "Optimizing ML Concurrent Computation and Communication with GPU DMA Engines." ISPASS 2025.
4. [[arXiv:2401.16677]](https://arxiv.org/abs/2401.16677) Pati et al. "T3: Transparent Tracking & Triggering for Fine-grained Overlap of Compute & Collectives." ASPLOS 2024.

## Acknowledgements

The GPU queue management infrastructure (SDMAQueue, BaseQueue, SDMAPacket, and related utilities) is adapted from [kfdtest](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocr-runtime/rocrtst), copyright Advanced Micro Devices, Inc., licensed under the MIT License.

---

*AMD, Instinct, ROCm, and related marks are trademarks of Advanced Micro Devices, Inc. This software is provided for research and evaluation purposes.*
