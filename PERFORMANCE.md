# AMD DMA-Latte — Performance Guide

This document describes the SDMA packet types used by each collective variant, relative performance of each variant, and DMA command phase breakdown on MI300X.

---

## SDMA Packet Types

Each collective is built from a small set of SDMA packets issued directly to xGMI sDMA engines via the HSAKMT ring buffer.

### Linear Copy

`SDMA_PKT_COPY_LINEAR` — copies bytes from a source GPU address to a destination GPU address.

### Broadcast Copy

`SDMA_PKT_COPY_LINEAR` with the broadcast flag set — a single command writes the same source data to **two** destination GPU addresses simultaneously, halving the number of commands and completion signals needed for an all-gather relative to parallel copy.

### Swap

`SDMA_PKT_SWAP_LINEAR` — bidirectional in-place exchange. Swaps the contents of two GPU buffers in a single DMA command, covering both directions of an all-to-all pair in one packet.

### Atomic

`SDMA_PKT_ATOMIC` — 64-bit atomic add to a signal location. Used as a completion notification — we add −1 to a signal initialised to 1, and the CPU spins until it reads 0.

A lighter alternative is a 32-bit fence write (`SDMA_PKT_FENCE`, `SYNC_TYPE=0/1`).

### Poll

`SDMA_PKT_POLL_REGMEM` — GPU-side poll on a system-memory signal. Stalls the DMA engine until the signal value matches a reference. Used with `PRELAUNCH=1` to gate command execution until the CPU writes a trigger — decoupling command scheduling from execution and removing the former from the critical path of the copy/collective.

### Timestamp

`SDMA_PKT_TIMESTAMP` — records the current GPU global timestamp into system memory. Used to capture sDMA command timing.

---

## Per-Variant Packet Strategy

| Variant | Copy commands per GPU | Sync commands per GPU | Notes |
|---------|----------------------|----------------------|-------|
| `pcpy`  | N−1                  | N−1                  | One engine per (src, dst) pair; all in parallel |
| `bcst`  | ⌈(N−1)/2⌉            | ⌈(N−1)/2⌉            | Each broadcast command covers 2 destinations |
| `swap`  | (N−1)/2              | (N−1)/2              | One swap per GPU pair covers both transfer directions |
| `b2b`   | N−1 on 1 engine      | 1                    | All destinations on one engine; single sync for the batch |

Lower command count reduces scheduling and sync overheads, which dominate at small transfer sizes (see [DMA-Latte paper](https://arxiv.org/abs/2511.06605), Section 3).

---

## DMA Command Latency Breakdown

### DMA Copy Phases

A single DMA copy command traverses four phases, measured using `COLLECTIVE_TYPE=p2p VARIANT=latency` with combined GPU and CPU timestamps:

- **control** — CPU creates and enqueues the DMA commands
- **schedule** — CPU rings doorbell and DMA engine fetches commands
- **copy** — DMA transfer
- **sync** — Completion signal propagates back to CPU

<img width="850" height="384" alt="image" src="https://github.com/user-attachments/assets/dce5da7e-7bc5-461f-960b-07dc1d9c5bd1" />



### MI300X Results

| Size   | control | schedule | copy | sync | non-copy total |
|--------|---------|----------|------|------|----------------|
| 1 KB   | 5%  | 31%  | 29%  | 35%  | **71%** |
| 2 KB   | 4%  | 31%  | 29%  | 35%  | **71%** |
| 4 KB   | 5%  | 30%  | 30%  | 36%  | **70%** |
| 8 KB   | 4%  | 31%  | 30%  | 35%  | **70%** |
| 16 KB  | 4%  | 30%  | 33%  | 33%  | **67%** |
| 32 KB  | 4%  | 28%  | 36%  | 32%  | **64%** |
| 64 KB  | 4%  | 23%  | 46%  | 27%  | **54%** |
| 128 KB | 7%  | 20%  | 51%  | 22%  | **49%** |
| 256 KB | 3%  | 17%  | 62%  | 18%  | **38%** |
| 512 KB | 2%  | 13%  | 72%  | 13%  | **28%** |
| 1 MB   | 1%  | 10%  | 79%  | 10%  | **21%** |
| 2 MB   | 1%  | 5%   | 89%  | 5%   | **11%** |
| 4 MB   | 1%  | 3%   | 93%  | 3%   | **7%**  |
| 8 MB   | 0%  | 1%   | 97%  | 1%   | **3%**  |
| 16 MB  | 0%  | 1%   | 98%  | 1%   | **2%**  |
| 32 MB  | 0%  | 0%   | 99%  | 0%   | **1%**  |
| 64 MB  | 0%  | 0%   | 100% | 0%   | **\<1%** |

Non-copy phases dominate at small sizes; 71% of time for a 1 KB copy. This is the primary motivation for `PRELAUNCH=1` (removes control and schedule from the critical path) and for collective variants that reduce command count (`bcst`, `swap`, `b2b`).

---

## Collective Performance

All-gather and all-to-all normalized latency across transfer sizes on a MI300X node with 8 GPUs in SPX mode. Run with engine affinity map (`SDMA_MAP`), `SYNC_TYPE=2`, `NUM_ITER=20`.

### All-Gather

PL = `PRELAUNCH=1`. Speedup relative to `pcpy` (higher = better). Values >1× mean that variant is faster than pcpy at that size.

| Size  | pcpy  | pcpy+PL | bcst  | bcst+PL | b2b   | b2b+PL | best  |
|-------|-------|---------|-------|---------|-------|--------|-------|
| 1KB   | 1.00× | 2.87×   | 1.59× | 3.67×   | 2.58× | 4.10×  | 4.10× |
| 2KB   | 1.00× | 3.01×   | 1.67× | 3.83×   | 2.65× | 4.22×  | 4.22× |
| 4KB   | 1.00× | 2.90×   | 1.64× | 3.78×   | 2.59× | 4.13×  | 4.13× |
| 8KB   | 1.00× | 2.82×   | 1.64× | 3.81×   | 2.60× | 4.23×  | 4.23× |
| 16KB  | 1.00× | 3.17×   | 1.79× | 4.10×   | 2.81× | 4.47×  | 4.47× |
| 32KB  | 1.00× | 2.99×   | 1.78× | 3.95×   | 2.85× | 4.49×  | 4.49× |
| 64KB  | 1.00× | 3.21×   | 1.76× | 4.01×   | 2.84× | 4.46×  | 4.46× |
| 128KB | 1.00× | 3.48×   | 2.05× | 4.52×   | 3.07× | 4.58×  | 4.58× |
| 256KB | 1.00× | 3.59×   | 2.00× | 4.33×   | 2.64× | 3.58×  | 4.33× |
| 512KB | 1.00× | 3.31×   | 1.96× | 3.73×   | 2.05× | 1.58×  | 3.73× |
| 1MB   | 1.00× | 3.22×   | 1.75× | 2.93×   | 1.56× | 1.84×  | 3.22× |
| 2MB   | 1.00× | 2.71×   | 1.44× | 2.16×   | 1.00× | 1.13×  | 2.71× |
| 4MB   | 1.00× | 2.29×   | 1.10× | 1.23×   | 0.61× | 0.65×  | 2.29× |
| 8MB   | 1.00× | 1.83×   | 0.81× | 0.94×   | 0.39× | 0.40×  | 1.83× |
| 16MB  | 1.00× | 1.54×   | 0.67× | 0.75×   | 0.27× | 0.27×  | 1.54× |
| 32MB  | 1.00× | 1.24×   | 0.52× | 0.55×   | 0.19× | 0.20×  | 1.24× |
| 64MB  | 1.00× | 1.11×   | 0.46× | 0.48×   | 0.17× | 0.17×  | 1.11× |
| 128MB | 1.00× | 1.05×   | 0.44× | 0.45×   | 0.16× | 0.16×  | 1.05× |
| 256MB | 1.00× | 1.03×   | 0.43× | 0.44×   | 0.15× | 0.15×  | 1.03× |
| 512MB | 1.00× | 1.02×   | 0.43× | 0.43×   | 0.15× | 0.15×  | 1.02× |
| 1GB   | 1.00× | 1.02×   | 0.43× | 0.43×   | 0.15× | 0.15×  | 1.02× |
| 2GB   | 1.00× | 1.01×   | 0.42× | 0.43×   | 0.13× | 0.15×  | 1.01× |

### All-to-All

PL = `PRELAUNCH=1`. Speedup relative to `pcpy`.

| Size  | pcpy  | pcpy+PL | swap  | swap+PL | b2b   | b2b+PL | best  |
|-------|-------|---------|-------|---------|-------|--------|-------|
| 1KB   | 1.00× | 2.55×   | 1.81× | 3.81×   | 2.80× | 4.10×  | 4.10× |
| 2KB   | 1.00× | 2.58×   | 1.82× | 3.91×   | 2.84× | 4.09×  | 4.09× |
| 4KB   | 1.00× | 2.53×   | 1.85× | 3.96×   | 2.79× | 4.04×  | 4.04× |
| 8KB   | 1.00× | 2.49×   | 1.84× | 3.94×   | 2.76× | 4.16×  | 4.16× |
| 16KB  | 1.00× | 2.45×   | 1.79× | 3.86×   | 2.78× | 4.00×  | 4.00× |
| 32KB  | 1.00× | 2.37×   | 1.81× | 3.88×   | 2.66× | 3.98×  | 3.98× |
| 64KB  | 1.00× | 2.57×   | 1.83× | 3.76×   | 2.73× | 3.53×  | 3.76× |
| 128KB | 1.00× | 2.51×   | 1.78× | 3.93×   | 2.46× | 3.29×  | 3.93× |
| 256KB | 1.00× | 2.49×   | 1.77× | 3.58×   | 2.11× | 2.70×  | 3.58× |
| 512KB | 1.00× | 2.36×   | 1.67× | 3.18×   | 1.69× | 2.03×  | 3.18× |
| 1MB   | 1.00× | 2.23×   | 1.55× | 2.65×   | 1.22× | 1.41×  | 2.65× |
| 2MB   | 1.00× | 2.07×   | 1.37× | 2.16×   | 0.81× | 0.88×  | 2.16× |
| 4MB   | 1.00× | 1.86×   | 1.18× | 1.67×   | 0.52× | 0.54×  | 1.86× |
| 8MB   | 1.00× | 1.62×   | 1.00× | 1.22×   | 0.35× | 0.36×  | 1.62× |
| 16MB  | 1.00× | 1.48×   | 0.86× | 0.99×   | 0.26× | 0.27×  | 1.48× |
| 32MB  | 1.00× | 1.27×   | 0.73× | 0.79×   | 0.20× | 0.20×  | 1.27× |
| 64MB  | 1.00× | 1.16×   | 0.66× | 0.70×   | 0.17× | 0.17×  | 1.16× |
| 128MB | 1.00× | 1.12×   | 0.65× | 0.66×   | 0.16× | 0.16×  | 1.12× |
| 256MB | 1.00× | 1.06×   | 0.62× | 0.63×   | 0.15× | 0.15×  | 1.06× |
| 512MB | 1.00× | 1.03×   | 0.53× | 0.53×   | 0.15× | 0.15×  | 1.03× |
| 1GB   | 1.00× | 0.75×   | 0.53× | 0.52×   | 0.15× | 0.15×  | 1.00× |
| 2GB   | 1.00× | 1.01×   | 0.51× | 0.50×   | 0.14× | 0.15×  | 1.01× |

---
