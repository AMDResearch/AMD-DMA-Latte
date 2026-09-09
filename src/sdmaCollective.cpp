/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <utility>
#include "sdmaCollective.hpp"
#include "SDMAPacket.hpp"
#include "XgmiOptimizedSDMAQueue.hpp"
#include <algorithm>
#include <cassert>

#include <map>
#include <fstream> 
#include <set>

#include <xmmintrin.h>

#include <iostream>
#include <chrono>
#include <numeric>  

void sdmaCollective::SetUp() {
    sdmaBase::SetUp();
}

void sdmaCollective::TearDown() {
    sdmaBase::TearDown();
}

// ── Timing and output helpers ────────────────────────────────────────────────
void sdmaCollective::logTimes(int total_transfers, std::vector<std::chrono::high_resolution_clock::time_point>& times, bool poll){
    cpu_times.resize(total_transfers);
    for(int k=0;k<total_transfers;k++){ 
        if (poll)
            cpu_times[k].push_back(std::chrono::duration_cast<std::chrono::duration<double> >(times[k+3] - times[2]).count());
        else
            cpu_times[k].push_back(std::chrono::duration_cast<std::chrono::duration<double> >(times[k+2] - times[1]).count());
    }
    
    controlTime.push_back(std::chrono::duration_cast<std::chrono::duration<double> >(times[1] - times[0]).count());
    if (poll)
        scheduleCopyTime.push_back(std::chrono::duration_cast<std::chrono::duration<double> >(times[total_transfers+2] - times[2]).count());
    else
        scheduleCopyTime.push_back(std::chrono::duration_cast<std::chrono::duration<double> >(times[total_transfers+1] - times[1]).count());
}

void sdmaCollective::finalizeTime(HSAuint64 dataBufSize, int total_transfers, bool poll, int num_iter){
    // Require at least 6 iters (5 warmup + 1 measurement).
    if (num_iter <= 5) {
        std::cerr << "[WARN] finalizeTime: num_iter=" << num_iter
                  << " <= 5 warmup; skipping output (need at least 6 iters).\n";
        return;
    }

    double control_sum = std::accumulate(controlTime.begin()+5, controlTime.end(), 0.0);
    double control_average = (static_cast<double>(control_sum) / (controlTime.size()-5)) * 1e9;

    double schedule_copy_time_sum = std::accumulate(scheduleCopyTime.begin()+5, scheduleCopyTime.end(), 0.0);
    double schedule_copy_time_avg = (static_cast<double>(schedule_copy_time_sum) / (scheduleCopyTime.size()-5)) * 1e9;

    double total = poll ? schedule_copy_time_avg : control_average + schedule_copy_time_avg;

    HSAuint64 total_bytes = dataBufSize * numNodes;
    double time_us  = total * 1e-3;                          // ns → us
    double algbw    = (double)total_bytes / total;           // B/ns == GB/s

    // algbw: (N-1) chunks actually move per GPU (own chunk stays local).
    // busbw: per-link bandwidth — each of the N-1 links carries 1 chunk.
    // With N GPUs: algbw = (N-1)/N × total_bytes / time;
    //              busbw = algbw / (N-1) = total_bytes / (N × time).
    double algbw_corrected = (numNodes > 1)
        ? algbw * (double)(numNodes - 1) / numNodes
        : algbw;
    double busbw = (numNodes > 0) ? (double)total_bytes / numNodes / (time_us * 1e-6) / 1e9 : 0;

    // Default output: transfer size, latency, algorithmic BW, per-link BW
    std::cout << std::dec << total_bytes << ","
              << std::fixed << std::setprecision(2)
              << time_us         << ","
              << algbw_corrected << ","
              << busbw           << std::endl;

    // With SDMA_VERBOSE: also print control overhead and per-transfer breakdown
    if (g_verbose) {
        std::cout << "  control(ns)=" << std::fixed << std::setprecision(0) << control_average
                  << " schedule_copy(ns)=" << schedule_copy_time_avg;
        for (int k = 0; k < total_transfers; k++) {
            double sum = std::accumulate(cpu_times[k].begin()+5, cpu_times[k].end(), 0.0);
            double avg = (static_cast<double>(sum) / (cpu_times[k].size()-5)) * 1e9;
            std::cout << " xfer" << k << "(ns)=" << avg;
        }
        std::cout << std::endl;
    }

    sizeTimeMap[(dataBufSize*numNodes)] = total;
    controlTime.clear();
    scheduleCopyTime.clear();
    for(int k=0;k<total_transfers;k++){
        cpu_times[k].clear();
    }
}

void sdmaCollective::freeMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter){
    HSAuint64 alloc_size=dataBufSize*numNodes;
    for (int i=0; i < numNodes; i++){
        EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(sysMem[i][0]), "unmapping failed!");
        EXPECT_SUCCESS(hsaKmtFreeMemory(sysMem[i][0], alloc_size), "freeing memory failed!");
    }
    for (int i=0; i < numNodes; i++){
        for (int iter=0; iter < num_iter; iter++){
            EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(gpuMem[i][iter]), "unmapping failed!");
            EXPECT_SUCCESS(hsaKmtFreeMemory(gpuMem[i][iter], alloc_size), "freeing memory failed!");
        }
    }
    for (int iter=0; iter < num_iter; iter++){
        EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(signalMem[iter]), "unmapping failed!");
        EXPECT_SUCCESS(hsaKmtFreeMemory(signalMem[iter], signalBufSize), "freeing memory failed!");
    }
}

void sdmaCollective::freeMemP2p(HSAuint64 size, HSAuint64 signalBufSize, int sig_iters, int buf_iters, int src, int dst) {
    HSAuint64 alloc_size = (size < (4ULL << 10)) ? (4ULL << 10) : size;
    for (int i : {src, dst}) {
        if (!sysMem[i].empty() && sysMem[i][0]) {
            EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(sysMem[i][0]), "sysMem unmap failed");
            EXPECT_SUCCESS(hsaKmtFreeMemory(sysMem[i][0], alloc_size), "sysMem free failed");
        }
        for (int k = 0; k < buf_iters; k++) {
            if (!gpuMem[i].empty() && gpuMem[i][k]) {
                EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(gpuMem[i][k]), "gpuMem unmap failed");
                EXPECT_SUCCESS(hsaKmtFreeMemory(gpuMem[i][k], alloc_size), "gpuMem free failed");
            }
            if (!gpuOutMem[i].empty() && gpuOutMem[i][k]) {
                EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(gpuOutMem[i][k]), "gpuOutMem unmap failed");
                EXPECT_SUCCESS(hsaKmtFreeMemory(gpuOutMem[i][k], alloc_size), "gpuOutMem free failed");
            }
        }
    }
    for (int k = 0; k < sig_iters; k++) {
        EXPECT_SUCCESS(hsaKmtUnmapMemoryToGPU(signalMem[k]), "signalMem unmap failed");
        EXPECT_SUCCESS(hsaKmtFreeMemory(signalMem[k], signalBufSize), "signalMem free failed");
    }
}

void sdmaCollective::destroyQueues(){
    for (int i=0; i< numNodes; i++){
        for (int j=2; j< numTotalSdma; j++){
            EXPECT_SUCCESS(xgmiSdmaQueue[i][j].Destroy(), "queue detroy failed!");
        }
    }

    for (int i=0; i< numNodes; i++){
        for (int j=0; j< numCpuSdma; j++){
            CHECK_HSAKMT_SUCCESS(sdmaQueue[i][j].Destroy(), "Queue creation failed!");
        }
    }
}

// ── Initialisation ───────────────────────────────────────────────────────────
// Creates SDMA queues for all GPUs, validates engine counts, parses engine affinity map.
void sdmaCollective::initialize(HSAuint64 min_size, HSAuint64 max_size, std::vector<int> collective_gpus, std::ifstream* engineMapFile, bool sdmaQueueSize){
    allGpuNodes = m_NodeInfo.GetNodesWithGPU();
    numTotalNodes=allGpuNodes.size();
    numNodes=collective_gpus.size();
    minSize=min_size/numNodes;
    maxSize=max_size/numNodes;
    engineMap=engineMapFile;
    gpus=collective_gpus;

    if (numTotalNodes < numNodes) {
        std::cout << "Skipping execution: Available #GPUs: " << numTotalNodes << " < Required GPUs: " << numNodes << std::endl;
        return;
    }

    const HsaNodeProperties *nodeProps = m_NodeInfo.GetNodeProperties(allGpuNodes[0]);
    numCpuSdma   = nodeProps->NumSdmaEngines;
    numTotalSdma = nodeProps->NumSdmaEngines + nodeProps->NumSdmaXgmiEngines;

    std::string gfxArch = GfxArchString(nodeProps);

    // Validate engine count against known arch expectations.
    // gfx942 (MI300X) requires SPX compute partition mode (16 total sDMA engines per GPU).
    // Fewer engines means the system is in a partitioned mode (e.g. QPX/CPX) which is not
    // yet supported — collective performance has not been characterized in those modes.
    if (gfxArch == "gfx942" && numTotalSdma < 16) {
        std::cerr << "[ERROR] " << gfxArch << " (MI3xx): expected 16 sDMA engines per GPU "
                  << "(2 CPU + 14 xGMI) but found " << numTotalSdma << ".\n"
                  << "        This tool requires SPX compute partition mode.\n"
                  << "        Run: rocm-smi --setcomputepartition=SPX\n";
        exit(1);
    }

    if (g_verbose) {
        std::cerr << "[INFO] " << gfxArch << ": numCpuSdma=" << numCpuSdma
                  << " numTotalSdma=" << numTotalSdma << "\n";
    }

    // gfx942 (MI300X) has per-engine destination affinity; warn if no map provided.
    // gfx90a (MI210) has no engine affinity; map is not needed and is ignored if set.
    if (gfxArch == "gfx942" && !engineMapFile) {
        std::cerr << "[WARN] SDMA_MAP not set on " << gfxArch << " (MI300X): falling back to "
                  << "round-robin engine assignment. Performance may be significantly worse.\n"
                  << "       Run scripts/gen_sdma_map_sysfs.py to generate the map for this machine.\n";
    }

    gpuNodes.resize(numNodes);
    hwToLocal.clear();
    for (int n = 0; n < numNodes; n++) {
        gpuNodes[n] = allGpuNodes[collective_gpus[n]];
        hwToLocal[collective_gpus[n]] = n;
    }

    xgmiSdmaQueue.resize(numTotalNodes);
    sdmaQueue.resize(numTotalNodes);

    for (int i=0; i< numTotalNodes; i++){
        sdmaQueue[i].resize(numCpuSdma);
        for (int j=0; j< numCpuSdma; j++){
            CHECK_HSAKMT_SUCCESS(sdmaQueue[i][j].Create(allGpuNodes[i]), "Queue creation failed!");
        }
    }

    for (int i=0; i< numTotalNodes; i++){
        xgmiSdmaQueue[i].resize(numTotalSdma);
        for (int j=numCpuSdma; j< numTotalSdma; j++){
            xgmiQueueSize = sdmaQueueSize ? (1<<21) : (4<<10);
            CHECK_HSAKMT_SUCCESS(xgmiSdmaQueue[i][j].Create(allGpuNodes[i],xgmiQueueSize), "Queue creation failed!");
        }
    }
}



// Allocate GPU, system, and signal buffers for all GPUs.
void sdmaCollective::allocateMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter){
    sysMem.resize(numNodes);
    sysBufs.resize(numNodes);
    gpuMem.resize(numNodes);
    gpuBufs.resize(numNodes);
    gpuOutMem.resize(numNodes);
    gpuOutBufs.resize(numNodes);

    // Each GPU allocates one contiguous slab of size (dataBufSize * numNodes).
    // For small transfers below 4KB (minimum page size), round up to avoid sub-page allocations.
    HSAuint64 total_size = dataBufSize * numNodes;
    HSAuint64 alloc_size = (total_size < (4ULL << 10)) ? (4ULL << 10) : total_size;

    // System (CPU-pinned) buffer: one per GPU, reused across iterations for validation readback.
    // sysBufs[i][0][j] points into GPU i's slab at chunk offset j (chunk size = dataBufSize).
    HsaMemFlags sysMemFlags = {0};
    HsaMemMapFlags sysMapFlags = {0};
    sysMemFlags.ui32.PageSize = HSA_PAGE_SIZE_4KB;
    sysMemFlags.ui32.HostAccess = 1;

    for (int i=0; i< numNodes; i++){
        sysMem[i].resize(1);
        sysBufs[i].resize(1);
        EXPECT_SUCCESS(hsaKmtAllocMemory(0, alloc_size, sysMemFlags,(void **)&sysMem[i][0]), "memory alloc failed!");
        EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(sysMem[i][0], alloc_size, NULL, sysMapFlags, 1, (HSAuint32*)&gpuNodes[i]), "memory mapping failed!");
        memset(sysMem[i][0], 0, alloc_size);
        sysBufs[i][0].resize(numNodes);
        for (int j=0; j< numNodes; j++)
            sysBufs[i][0][j]=sysMem[i][0] + ((dataBufSize/sizeof(HSAuint32)) *j);
    }

    // GPU (HBM) buffers: one input slab (gpuMem) and one output slab (gpuOutMem) per GPU
    // per iteration, both mapped peer-accessible to all GPUs in the collective.
    // gpuBufs[i][k][j] / gpuOutBufs[i][k][j] point to chunk j within GPU i's slab.
    HsaMemFlags gpuMemFlags = {0};
    HsaMemMapFlags gpuMapFlags = {0};
    gpuMemFlags.ui32.PageSize = HSA_PAGE_SIZE_4KB;
    gpuMemFlags.ui32.HostAccess = 0;
    gpuMemFlags.ui32.NonPaged = 1;
    gpuMemFlags.ui32.NoNUMABind = 1;

    for (int i=0; i< numNodes; i++){
        gpuMem[i].resize(num_iter);
        gpuBufs[i].resize(num_iter);
        gpuOutMem[i].resize(num_iter);
        gpuOutBufs[i].resize(num_iter);
        for (int k=0; k< num_iter; k++){
            CHECK_HSAKMT_SUCCESS(hsaKmtAllocMemory(gpuNodes[i], alloc_size, gpuMemFlags, reinterpret_cast<void**>(&gpuMem[i][k])),"memory allocation failed!");
            CHECK_HSAKMT_SUCCESS(hsaKmtMapMemoryToGPU(gpuMem[i][k], alloc_size, NULL),"memory mapping failed!");
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(gpuMem[i][k], alloc_size, NULL, gpuMapFlags, numNodes, (HSAuint32 *)&gpuNodes[0]),"memory mapping to multiple GPUs failed!");
            CHECK_HSAKMT_SUCCESS(hsaKmtAllocMemory(gpuNodes[i], alloc_size, gpuMemFlags, reinterpret_cast<void**>(&gpuOutMem[i][k])),"memory allocation failed!");
            CHECK_HSAKMT_SUCCESS(hsaKmtMapMemoryToGPU(gpuOutMem[i][k], alloc_size, NULL),"memory mapping failed!");
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(gpuOutMem[i][k], alloc_size, NULL, gpuMapFlags, numNodes, (HSAuint32 *)&gpuNodes[0]),"memory mapping to multiple GPUs failed!");
            gpuBufs[i][k].resize(numNodes);
            gpuOutBufs[i][k].resize(numNodes);
            for (int j=0; j< numNodes; j++){
                gpuBufs[i][k][j]=reinterpret_cast<HSAuint32*>(gpuMem[i][k]) + ((dataBufSize/sizeof(HSAuint32)) *j);
                gpuOutBufs[i][k][j]=reinterpret_cast<HSAuint32*>(gpuOutMem[i][k]) + ((dataBufSize/sizeof(HSAuint32)) *j);
            }
        }
    }

    // Signal buffers: CPU-pinned, mapped to all GPUs — used for DMA completion signalling.
    // One buffer per iteration; slots within the buffer are indexed by (gpu * num_cmds + cmd).
    signalMem.resize(num_iter);
    for (int k=0; k< num_iter; k++){
        EXPECT_SUCCESS(hsaKmtAllocMemory(0, signalBufSize, sysMemFlags,(void **)&signalMem[k]), "memory alloc failed!");
        EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(signalMem[k], signalBufSize, NULL, sysMapFlags, numNodes, (HSAuint32*)&gpuNodes[0]), "memory mapping failed!");
    }

}

// P2p-only allocation: size is the direct transfer size (no ×numNodes).
// Only allocates buffers for src and dst GPUs, mapped only to those two nodes.
// sig_iters: number of signal buffer slots (defaults to num_iter; pass num_iter always for timing loops).
void sdmaCollective::allocateMemP2p(HSAuint64 size, HSAuint64 signalBufSize, int num_iter, int src, int dst, int sig_iters) {
    if (sig_iters <= 0) sig_iters = num_iter;
    sysMem.resize(numNodes);
    sysBufs.resize(numNodes);
    gpuMem.resize(numNodes);
    gpuBufs.resize(numNodes);
    gpuOutMem.resize(numNodes);
    gpuOutBufs.resize(numNodes);

    HSAuint64 alloc_size = (size < (4ULL << 10)) ? (4ULL << 10) : size;

    HsaMemFlags sysFlags = {0};
    HsaMemMapFlags sysMapFlags = {0};
    sysFlags.ui32.PageSize  = HSA_PAGE_SIZE_4KB;
    sysFlags.ui32.HostAccess = 1;

    HsaMemFlags gpuFlags = {0};
    HsaMemMapFlags gpuMapFlags = {0};
    gpuFlags.ui32.PageSize  = HSA_PAGE_SIZE_4KB;
    gpuFlags.ui32.HostAccess = 0;
    gpuFlags.ui32.NonPaged  = 1;
    gpuFlags.ui32.NoNUMABind = 1;

    for (int i : {src, dst}) {
        int peer = (i == src) ? dst : src;
        HSAuint32 p2p_nodes[2] = { (HSAuint32)gpuNodes[src], (HSAuint32)gpuNodes[dst] };

        sysMem[i].resize(1);
        sysBufs[i].resize(1);
        EXPECT_SUCCESS(hsaKmtAllocMemory(0, alloc_size, sysFlags, (void**)&sysMem[i][0]), "sysMem alloc failed");
        EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(sysMem[i][0], alloc_size, NULL, sysMapFlags, 1, (HSAuint32*)&gpuNodes[i]), "sysMem map failed");
        memset(sysMem[i][0], 0, alloc_size);
        sysBufs[i][0].resize(numNodes);
        sysBufs[i][0][peer] = sysMem[i][0];

        gpuMem[i].resize(num_iter);
        gpuBufs[i].resize(num_iter);
        gpuOutMem[i].resize(num_iter);
        gpuOutBufs[i].resize(num_iter);
        for (int k = 0; k < num_iter; k++) {
            CHECK_HSAKMT_SUCCESS(hsaKmtAllocMemory(gpuNodes[i], alloc_size, gpuFlags, reinterpret_cast<void**>(&gpuMem[i][k])), "gpuMem alloc failed");
            CHECK_HSAKMT_SUCCESS(hsaKmtMapMemoryToGPU(gpuMem[i][k], alloc_size, NULL), "gpuMem map failed");
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(gpuMem[i][k], alloc_size, NULL, gpuMapFlags, 2, p2p_nodes), "gpuMem p2p map failed");
            CHECK_HSAKMT_SUCCESS(hsaKmtAllocMemory(gpuNodes[i], alloc_size, gpuFlags, reinterpret_cast<void**>(&gpuOutMem[i][k])), "gpuOutMem alloc failed");
            CHECK_HSAKMT_SUCCESS(hsaKmtMapMemoryToGPU(gpuOutMem[i][k], alloc_size, NULL), "gpuOutMem map failed");
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(gpuOutMem[i][k], alloc_size, NULL, gpuMapFlags, 2, p2p_nodes), "gpuOutMem p2p map failed");
            gpuBufs[i][k].resize(numNodes);
            gpuOutBufs[i][k].resize(numNodes);
            gpuBufs[i][k][peer]    = gpuMem[i][k];
            gpuOutBufs[i][k][peer] = gpuOutMem[i][k];
        }
    }

    // Signal memory always gets num_iter slots regardless of buf reuse — timing loops use signalMem[iter].
    signalMem.resize(sig_iters);
    HSAuint32 sig_nodes[2] = { (HSAuint32)gpuNodes[src], (HSAuint32)gpuNodes[dst] };
    for (int k = 0; k < sig_iters; k++) {
        EXPECT_SUCCESS(hsaKmtAllocMemory(0, signalBufSize, sysFlags, (void**)&signalMem[k]), "signalMem alloc failed");
        EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(signalMem[k], signalBufSize, NULL, sysMapFlags, 2, sig_nodes), "signalMem map failed");
    }
}

// Fill GPU input buffers with deterministic patterns; zero output buffers.
void sdmaCollective::fillMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter){
    HSAuint64 total_size = dataBufSize * numNodes; // per GPU mem size chunks * gpus. Not total, but number of GPUs in collective.
    HSAuint64 alloc_size; // how much to allocate per GPU -- different if total_size < 4KB (page size)
    if (total_size < (4ULL << 10))
        alloc_size=4ULL << 10;
    else
        alloc_size=total_size;

    for (int i=0; i< numNodes; i++)
        memset(sysMem[i][0], 0, alloc_size);

    int signal_id;
    HSAuint64 value = 1;
    int sdma_id=2; // first xGMI engine (engines 0,1 are for CPU-GPU transfers)

    for (int iter=0; iter < num_iter; iter++){
        // Arm one signal slot per GPU; fence decrements it when fills complete.
        for (int i=0; i< numNodes; i++){
            signal_id = (i * numNodes);
            __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
        }

        // Fill input buffers (gpuBufs) with pattern mem_id = i*numNodes+j, encoding the
        // owning GPU and chunk index — makes validation failures easy to diagnose.
        // Output buffers (gpuOutBufs): pre-fill the diagonal (own chunk, i==j) with the same
        // pattern; zero all off-diagonal slots (destinations to be written by the collective).
        for (int i=0; i< numNodes; i++){
            for (int j=0; j< numNodes; j++){
                HSAuint32 mem_id= (i * numNodes) + j;
                xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMAFillDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuBufs[i][iter][j], mem_id, dataBufSize));
                if (i==j)
                    xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMAFillDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), reinterpret_cast<HSAuint64*>(gpuOutBufs[i][iter][j]), mem_id, dataBufSize));
                else
                    xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMAFillDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), reinterpret_cast<HSAuint64*>(gpuOutBufs[i][iter][j]), 0, dataBufSize));
            }
            signal_id = (i * numNodes);
            xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMAFencePacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), &signalMem[iter][signal_id], 0));
            xgmiSdmaQueue[i][sdma_id].SubmitPacket();
        }

        while(true){
            int done=0;
            for (int i=0; i< numNodes; i++){
                signal_id = (i * numNodes);
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0)
                    done++;
            }
            if (done==numNodes)
                break;
        }
    }
}

// Validate collective results against expected patterns.
void sdmaCollective::validateMem(std::string collective, HSAuint64 dataBufSize, bool inplace, int num_iter){
    int sdma_id=0; // CPU-GPU engine (engine 0) — used for readback, not on the xGMI data path
    int signal_id=0;
    HSAuint64 value = 1;

    // DMA-copy GPU output buffers to system memory, then check element-by-element on CPU.
    // inplace=true: collective wrote results into gpuBufs (e.g. ag, swap).
    // inplace=false: collective wrote results into gpuOutBufs (e.g. aa pcpy).
    for (int iter=0; iter < num_iter; iter++){
        for (int i=1; i< numNodes; i++){
            signal_id = (i * numNodes);
            __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
        }

        for (int i=1; i<numNodes;i++){
            for (int j=0; j<numNodes;j++){
                if(inplace)
                    sdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(sdmaQueue[i][sdma_id].GetFamilyId(),sysBufs[i][0][j],gpuBufs[i][iter][j],dataBufSize));
                else
                    sdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(sdmaQueue[i][sdma_id].GetFamilyId(),sysBufs[i][0][j],gpuOutBufs[i][iter][j],dataBufSize));
            }
            signal_id = (i * numNodes);
            sdmaQueue[i][sdma_id].PlacePacket(SDMAFencePacket(sdmaQueue[i][sdma_id].GetFamilyId(), &signalMem[iter][signal_id], 0));
            sdmaQueue[i][sdma_id].SubmitPacket();
        }

        while(true){
            int done=0;
            for (int i=1; i< numNodes; i++){
                signal_id = (i * numNodes);
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0)
                    done++;
            }
            if (done==(numNodes-1))
                break;
        }

        HSAuint64 expected_val = 0;
        for (int i=1; i< numNodes; i++){
            for (int j=0; j< numNodes; j++){
                int end = dataBufSize / sizeof(HSAuint64) - 1;
                if (collective == "aa")
                    expected_val = i + (j * numNodes); // for alltoall
                if (collective == "ag")
                    expected_val = j + (j * numNodes); // for allgather
                for (int k=0; k < end; k++){
                    if (sysBufs[i][0][j][k] != expected_val){
                        std::cout << "sysBufs elements for gpu " << i << " buf " << j << " and element " << k << " is " << sysBufs[i][0][j][k] << std::endl;
                        std::cout << std::dec << "for size: "<< dataBufSize << ", GPU " << i << " to GPU " << j << " FAILED" << std::endl;
                    }
                    CHECK_EQ(sysBufs[i][0][j][k], expected_val, "validation failed!");
                }
            }
        }
    }
}

void sdmaCollective::getPreferredEngines(bool sdma_to_dest){
    if (!engineMap || !engineMap->is_open()) {
        if (sdma_to_dest) {
            // No SDMA_MAP: round-robin engine assignment.
            // On MI300X this is suboptimal; on MI210 all engines are equivalent.
            std::cerr << "[WARN] No SDMA_MAP: AllgatherBcst using round-robin engines.\n"
                      << "       For best performance on MI300X run:\n"
                      << "       python3 scripts/gen_sdma_map_sysfs.py --out engineMap.csv\n";
        }
        // No map (e.g. gfx90a/MI210): assign xGMI sDMA engines round-robin across destinations.
        // MI300X: use the even-numbered ones.
        int num_even_xgmi_sdma = (numTotalSdma - numCpuSdma) / 2;
        for (int i = 0; i < numTotalNodes; i++) {
            int dest_idx = 0;
            for (int j = 0; j < numTotalNodes; j++) {
                if (i == j) continue;
                int engine = numCpuSdma + 2 * (dest_idx % num_even_xgmi_sdma);
                if (sdma_to_dest)
                    preferredEngines[{i, engine}] = j;  // {src, engine} → dst (bcst format)
                else
                    preferredEngines[{i, j}] = engine;  // {src, dst} → engine (pcpy format)
                dest_idx++;
            }
        }
        return;
    }

    std::string line;
    while (std::getline(*engineMap, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string srcStr, destStr, engineStr;

        // col0: source GPU index
        if (!std::getline(ss, srcStr, ',')) continue;

        // col1: engine ID (sDMA engine on src GPU)
        if (!std::getline(ss, engineStr, ',')) continue;

        // col2: destination GPU index (the GPU this engine has affinity to)
        if (!std::getline(ss, destStr, ',')) continue;

        int srcIdx = std::stoi(srcStr);
        int destIdx = std::stoi(destStr);
        uint32_t engineID = std::stoul(engineStr);
        
        if (sdma_to_dest)
            preferredEngines[{srcIdx, engineID}] = destIdx;
        else
            preferredEngines[{srcIdx, destIdx}] = engineID;
    }
}

// Place the appropriate sync packet based on sync type:
//   0 = 32b fence, 1 = 2x 32b fences (hi then lo), 2 = 64b atomic decrement
void sdmaCollective::placeSyncPacket(XgmiOptimizedSDMAQueue& queue, HSAuint64* signal, int sync) {
    if (sync == 0) {
        queue.PlacePacket(SDMAFencePacket(queue.GetFamilyId(), signal, 0));
    } else if (sync == 1) {
        uint32_t* signal_lower  = reinterpret_cast<uint32_t*>(signal);
        uint32_t* signal_higher = signal_lower + 1;
        queue.PlacePacket(SDMAFencePacket(queue.GetFamilyId(), signal_higher, 0));
        queue.PlacePacket(SDMAFencePacket(queue.GetFamilyId(), signal_lower,  0));
    } else {
        queue.PlacePacket(SDMAAtomicPacket(queue.GetFamilyId(), signal));
    }
}

// Initialize signal memory for one iteration.
// Each (gpu, cmd) pair gets a signal slot; poll slots are set if prelaunch.
void sdmaCollective::initSignals(int num_cmds, int iter, bool poll, bool multi_poll) {
    HSAuint64 value = 1;
    for (int i = 0; i < numNodes; i++) {
        for (int ci = 0; ci < num_cmds; ci++) {
            int signal_id = (i * num_cmds) + ci;
            __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
            if (poll & multi_poll) {
                int poll_id = (numNodes * num_cmds) + (i * num_cmds) + ci;
                __atomic_store(&signalMem[iter][poll_id], &value, __ATOMIC_RELEASE);
            }
        }
    }
    if (poll & !multi_poll)
        __atomic_store(&signalMem[iter][numNodes * num_cmds], &value, __ATOMIC_RELEASE);
}

// Place the poll packet that gates a prelaunch command on its trigger signal.
void sdmaCollective::placePollPacket(XgmiOptimizedSDMAQueue& queue, int num_cmds, int iter,
                                      int gpu, int cmd_idx, bool poll, bool multi_poll) {
    if (!poll) return;
    if (multi_poll) {
        int poll_id = (numNodes * num_cmds) + (gpu * num_cmds) + cmd_idx;
        queue.PlacePacket(SDMAPollPacket(queue.GetFamilyId(), &signalMem[iter][poll_id], 0));
    } else {
        queue.PlacePacket(SDMAPollPacket(queue.GetFamilyId(), &signalMem[iter][numNodes * num_cmds], 0));
    }
}

// Trigger prelaunch commands (write poll signals) and record trigger timestamp.
void sdmaCollective::triggerPoll(int num_cmds, int iter, bool poll, bool multi_poll,
                                  float trigger_time, int& t_index,
                                  std::vector<std::chrono::high_resolution_clock::time_point>& times) {
    if (!poll) return;
    HSAuint64 reset_value = 0;
    sleep(trigger_time);
    times[t_index++] = std::chrono::high_resolution_clock::now();
    if (multi_poll) {
        for (int i = 0; i < numNodes; i++)
            for (int ci = 0; ci < num_cmds; ci++) {
                int poll_id = (numNodes * num_cmds) + (i * num_cmds) + ci;
                __atomic_store(&signalMem[iter][poll_id], &reset_value, __ATOMIC_RELEASE);
            }
    } else {
        __atomic_store(&signalMem[iter][numNodes * num_cmds], &reset_value, __ATOMIC_RELEASE);
    }
}

// Spin-wait until all (gpu, cmd) signals are cleared, recording per-completion timestamps.
void sdmaCollective::spinWait(int num_cmds, int iter, std::vector<int>& dest_done,
                               std::vector<std::chrono::high_resolution_clock::time_point>& times,
                               int& t_index, int done_target) {
    if (done_target < 0) done_target = num_cmds * numNodes;
    int done = 0;
    while (true) {
        for (int i = 0; i < numNodes; i++) {
            for (int ci = 0; ci < num_cmds; ci++) {
                int signal_id = (i * num_cmds) + ci;
                int64_t val;
                __atomic_load(&signalMem[iter][signal_id], &val, __ATOMIC_ACQUIRE);
                if (val == 0 && !dest_done[signal_id]) {
                    done++;
                    times[t_index++] = std::chrono::high_resolution_clock::now();
                    dest_done[signal_id] = 1;
                }
            }
        }
        if (done == done_target) break;
    }
}

// ── Collectives ──────────────────────────────────────────────────────────────
// All-gather: each GPU pushes its chunk to all peers; one engine per destination.
void sdmaCollective::AllgatherPcpy(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::vector<int> gpus, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    int num_dest = numNodes - 1;


    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++) {
        for (int j = 0; j < num_dest; j++) {
            int dest = (i + j + 1) % numNodes;
            cmds[i].push_back({(int)preferredEngines[{gpus[i], gpus[dest]}], {dest}});
        }
    }
    int num_cmds = (int)cmds[0].size(); // commands per GPU; for pcpy == num_dest

    if (g_verbose) {
        for (int i = 0; i < numNodes; i++)
            for (int ci = 0; ci < num_cmds; ci++)
                std::cerr << "GPU " << i << " cmd " << ci
                          << " engine=" << cmds[i][ci].engine_id
                          << " dest=" << cmds[i][ci].dests[0] << "\n";
    }

    HSAuint64 size;

    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> dest_done(num_cmds * numNodes, 0);
        HSAuint64 signalBufSize = 4ULL << 10;

        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);
            std::fill(dest_done.begin(), dest_done.end(), 0);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < num_cmds; ci++){
                    int signal_id = (i * num_cmds) + ci;
                    int sdma_id   = cmds[i][ci].engine_id;
                    int dest      = cmds[i][ci].dests[0];
                    if (g_verbose)
                        std::cerr << "GPU " << i << " cmd " << ci
                                  << " sdma=" << sdma_id << " dest=" << dest << "\n";
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuBufs[dest][iter][i], gpuBufs[i][iter][i], size));
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);
            spinWait(num_cmds, iter, dest_done, times, t_index);

            int total_transfers=numNodes*(numNodes-1);
            logTimes(total_transfers,times,poll);

        }

        bool inplace=true;
        validateMem("ag",size,inplace,num_iter);

        int total_transfers=numNodes*(numNodes-1);
        finalizeTime(size,total_transfers,poll,num_iter);
        freeMem(size, 4ULL << 10, num_iter);
    }

    std::string key = std::to_string(gpus[0]);
    for (int i=1; i < numNodes; i++){
        key = key+"-"+std::to_string(gpus[i]);
    }

    destroyQueues();

}


// All-to-all: each GPU pushes a unique chunk to every peer; one engine per (src, dst) pair.
void sdmaCollective::AlltoallPcpy(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    // Build command list: one copy per (src, dest) pair where src != dest
    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++)
        for (int j = 0; j < numNodes; j++)
            if (i != j)
                cmds[i].push_back({(int)preferredEngines[{gpus[i], gpus[j]}], {j}});
    int num_cmds = (int)cmds[0].size(); // = numNodes - 1

    HSAuint64 size;
    HSAuint64 signalBufSize = 4ULL << 10;
    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> dest_done(num_cmds * numNodes, 0);
        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::fill(dest_done.begin(), dest_done.end(), 0);
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < num_cmds; ci++){
                    int signal_id = (i * num_cmds) + ci;
                    int sdma_id   = cmds[i][ci].engine_id;
                    int dest      = cmds[i][ci].dests[0];
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuOutBufs[dest][iter][i], gpuBufs[i][iter][dest], size));
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);
            spinWait(num_cmds, iter, dest_done, times, t_index);

            int total_transfers = numNodes * (numNodes - 1);
            logTimes(total_transfers, times, poll);
        }
        bool inplace=false;
        validateMem("aa",size,inplace,num_iter);
        int total_transfers=numNodes*(numNodes-1);
        finalizeTime(size,total_transfers,poll,num_iter);
        freeMem(size, signalBufSize,num_iter);
    }

    std::string key = std::to_string(gpus[0]);
    for (int i=1; i < numNodes; i++){
        key = key+"-"+std::to_string(gpus[i]);
    }

    destroyQueues();
}

// All-to-all: multiple copies sent back-to-back on same engine to enable copy overlap in DMA.
void sdmaCollective::AlltoallNwayB2b(int num_iter, std::vector<int> dma, int ways, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    int num_dest = numNodes - 1;
    if ((int)dma.size() < num_dest / ways) {
        LOG() << "Skipping execution: DMA IDs specified less than required for this prototype\n";
        return;
    }

    // Build command list: group destinations into back-to-back batches of `ways` per engine
    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++) {
        int dma_id = 0;
        std::vector<int> batch;
        for (int j = 0; j < numNodes; j++) {
            if (i == j) continue;
            batch.push_back(j);
            if ((int)batch.size() == ways) {
                cmds[i].push_back({dma[dma_id++], batch});
                batch.clear();
            }
        }
        if (!batch.empty())
            cmds[i].push_back({dma[dma_id], batch});
    }
    int num_cmds = (int)cmds[0].size();

    HSAuint64 size;
    HSAuint64 signalBufSize = 4ULL << 10;
    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> dest_done(num_cmds * numNodes, 0);
        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::fill(dest_done.begin(), dest_done.end(), 0);
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < num_cmds; ci++){
                    int signal_id = (i * num_cmds) + ci;
                    int sdma_id   = cmds[i][ci].engine_id;
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    for (int dest : cmds[i][ci].dests)
                        xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuOutBufs[dest][iter][i], gpuBufs[i][iter][dest], size));
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);
            spinWait(num_cmds, iter, dest_done, times, t_index);

            int total_transfers = numNodes * num_cmds;
            logTimes(total_transfers, times, poll);
        }
        bool inplace = false;
        validateMem("aa", size, inplace, num_iter);
        int total_transfers = numNodes * num_cmds;
        finalizeTime(size, total_transfers, poll, num_iter);
        freeMem(size, signalBufSize, num_iter);
    }
    std::string key = std::to_string(gpus[0]);
    for (int i = 1; i < numNodes; i++)
        key = key + "-" + std::to_string(gpus[i]);
    destroyQueues();
}


// All-to-all: in-place bidirectional swap; one SDMA swap command covers both directions of a pair.
void sdmaCollective::AlltoallSwap(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    int total_commands    = numNodes * (numNodes - 1) / 2; // each swap covers 2 transfers
    int commands_per_node = total_commands % numNodes ? (total_commands / numNodes) + 1
                                                       : (total_commands / numNodes);

    // Greedily assign unique (i,j) swap pairs; each GPU gets up to commands_per_node (variable per GPU).
    std::set<std::pair<int,int>> seen;
    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++) {
        int gpu_cmds = 0;
        for (int j = 0; j < numNodes && gpu_cmds < commands_per_node; j++) {
            if (i == j) continue;
            auto p = std::minmax(gpus[i], gpus[j]); // deduplicate on hardware indices
            if (seen.count(p)) continue;
            seen.insert(p);
            cmds[i].push_back({(int)preferredEngines[{gpus[i], gpus[j]}], {j}});
            gpu_cmds++;
        }
    }
    int num_cmds = commands_per_node; // max per GPU; some GPUs have fewer

    HSAuint64 size;
    HSAuint64 signalBufSize = 4ULL << 10;
    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> dest_done(num_cmds * numNodes, 0);
        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::fill(dest_done.begin(), dest_done.end(), 0);
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            // Use cmds[i].size() not num_cmds — GPUs with fewer pairs have shorter lists
            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < (int)cmds[i].size(); ci++){
                    int signal_id = (i * num_cmds) + ci;
                    int sdma_id   = cmds[i][ci].engine_id;
                    int dest      = cmds[i][ci].dests[0];
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMASwapDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuBufs[dest][iter][i], gpuBufs[i][iter][dest], size));
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);
            // done_target = total_commands: some GPU signal slots stay at 1 (unused)
            spinWait(num_cmds, iter, dest_done, times, t_index, total_commands);

            logTimes(total_commands, times, poll);
        }
        bool inplace = true;
        validateMem("aa", size, inplace, num_iter);
        finalizeTime(size, total_commands, poll, num_iter);
        freeMem(size, signalBufSize, num_iter);
    }

    std::string key = std::to_string(gpus[0]);
    for (int i = 1; i < numNodes; i++)
        key = key + "-" + std::to_string(gpus[i]);
    destroyQueues();
}


// All-gather: each GPU broadcasts its chunk to 2 destinations per engine command.
void sdmaCollective::AllgatherBcst(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::vector<int> gpus, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    int num_dest = numNodes - 1;

    // Pair engines by sorted ID: lowest = single copy, rest pair consecutively for broadcast (2 dests).
    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++) {
        std::vector<std::pair<int,int>> engine_dests; // (engine_id, local_dest_idx)
        for (auto& [key, hw_dest] : preferredEngines)
            if (key.first == gpus[i])
                engine_dests.push_back({(int)key.second, hwToLocal[(int)hw_dest]});
        std::sort(engine_dests.begin(), engine_dests.end());

        // First destination: single copy (7 is odd, one must be unpaired)
        cmds[i].push_back({engine_dests[0].first, {engine_dests[0].second}});

        // Pair consecutive destinations into broadcast commands (2 destinations per command)
        for (int k = 1; k + 1 < (int)engine_dests.size(); k += 2)
            cmds[i].push_back({engine_dests[k+1].first,
                               {engine_dests[k].second, engine_dests[k+1].second}});

        // Even destination count: last destination unpaired, single copy
        if (engine_dests.size() % 2 == 0) {
            int last = (int)engine_dests.size() - 1;
            cmds[i].push_back({engine_dests[last].first, {engine_dests[last].second}});
        }
    }
    int num_cmds = (int)cmds[0].size(); // = total_commands

    HSAuint64 size;
    HSAuint64 signalBufSize = 4ULL << 10;
    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> node_done(numNodes * num_cmds, 0);
        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::fill(node_done.begin(), node_done.end(), 0);
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < num_cmds; ci++){
                    int sdma_id   = cmds[i][ci].engine_id;
                    int signal_id = (i * num_cmds) + ci;
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    if (cmds[i][ci].dests.size() == 1) {
                        int dest = cmds[i][ci].dests[0];
                        xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuBufs[dest][iter][i], gpuBufs[i][iter][i], size));
                    } else {
                        void *dst_array[] = {gpuBufs[cmds[i][ci].dests[0]][iter][i],
                                             gpuBufs[cmds[i][ci].dests[1]][iter][i]};
                        xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), dst_array, gpuBufs[i][iter][i], 2, size));
                    }
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);

            // Spinwait: each command completes dests.size() transfers (1=copy, 2=bcst)
            int done = 0;
            int64_t read_signal_acq;
            while (true){
                for (int i = 0; i < numNodes; i++){
                    for (int ci = 0; ci < num_cmds; ci++){
                        int signal_id = (i * num_cmds) + ci;
                        __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                        if (read_signal_acq == 0 && !node_done[signal_id]){
                            done += (int)cmds[i][ci].dests.size();
                            times[t_index++] = std::chrono::high_resolution_clock::now();
                            node_done[signal_id] = 1;
                        }
                    }
                }
                if (done == num_dest * numNodes) break;
            }

            logTimes(numNodes * num_cmds, times, poll);
        }

        bool inplace = true;
        validateMem("ag", size, inplace, num_iter);
        finalizeTime(size, numNodes * num_cmds, poll, num_iter);
        freeMem(size, 4ULL << 10, num_iter);
    }

    std::string key = std::to_string(gpus[0]);
    for (int i = 1; i < numNodes; i++)
        key = key + "-" + std::to_string(gpus[i]);
}


// All-gather: multiple copies sent back-to-back on same engine to enable copy overlap in DMA.
void sdmaCollective::AllgatherNwayB2b(int num_iter, std::vector<int> dma, int ways, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map) {
    std::cout <<"Collective: " <<  __func__ << std::endl;

    int num_dest = numNodes - 1;
    if ((int)dma.size() < num_dest / ways) {
        LOG() << "Skipping execution: DMA IDs specified less than required for this prototype\n";
        return;
    }

    // Build command list: group destinations into back-to-back batches of `ways` per engine
    std::vector<std::vector<sdmaCommand>> cmds(numNodes);
    for (int i = 0; i < numNodes; i++) {
        int dma_id = 0;
        std::vector<int> batch;
        for (int j = 0; j < numNodes; j++) {
            if (i == j) continue;
            batch.push_back(j);
            if ((int)batch.size() == ways) {
                cmds[i].push_back({dma[dma_id++], batch});
                batch.clear();
            }
        }
        if (!batch.empty())
            cmds[i].push_back({dma[dma_id], batch});
    }
    int num_cmds = (int)cmds[0].size();

    HSAuint64 size;
    HSAuint64 signalBufSize = 4ULL << 10;
    std::cout << std::dec << std::endl;
    std::cout << "size(B),time(us),algbw(GB/s),busbw(GB/s)" << std::endl;

    for (size = minSize; size <= maxSize; size <<= 1) {
        std::vector<int> dest_done(num_cmds * numNodes, 0);
        allocateMem(size, 4ULL << 10, num_iter);
        fillMem(size, signalBufSize, num_iter);

        for (int iter = 0; iter < num_iter; iter++){
            std::fill(dest_done.begin(), dest_done.end(), 0);
            std::vector<std::chrono::high_resolution_clock::time_point> times(58);

            initSignals(num_cmds, iter, poll, multi_poll);
            times[0] = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < numNodes; i++){
                for (int ci = 0; ci < num_cmds; ci++){
                    int signal_id = (i * num_cmds) + ci;
                    int sdma_id   = cmds[i][ci].engine_id;
                    placePollPacket(xgmiSdmaQueue[i][sdma_id], num_cmds, iter, i, ci, poll, multi_poll);
                    for (int dest : cmds[i][ci].dests)
                        xgmiSdmaQueue[i][sdma_id].PlacePacket(SDMACopyDataPacket(xgmiSdmaQueue[i][sdma_id].GetFamilyId(), gpuBufs[dest][iter][i], gpuBufs[i][iter][i], size));
                    placeSyncPacket(xgmiSdmaQueue[i][sdma_id], &signalMem[iter][signal_id], sync);
                    xgmiSdmaQueue[i][sdma_id].SubmitPacket();
                }
            }

            int t_index = 2;
            times[1] = std::chrono::high_resolution_clock::now();
            triggerPoll(num_cmds, iter, poll, multi_poll, trigger_time, t_index, times);
            spinWait(num_cmds, iter, dest_done, times, t_index);

            int total_transfers = numNodes * num_cmds;
            logTimes(total_transfers, times, poll);
        }
        bool inplace = true;
        validateMem("ag", size, inplace, num_iter);
        int total_transfers = numNodes * num_cmds;
        finalizeTime(size, total_transfers, poll, num_iter);
        freeMem(size, signalBufSize, num_iter);
    }
    std::string key = std::to_string(gpus[0]);
    for (int i = 1; i < numNodes; i++)
        key = key + "-" + std::to_string(gpus[i]);
    destroyQueues();
}

// Microbenchmark to capture finegrained sDMA copy latency using GPU and CPU timestamps.
void sdmaCollective::P2pCopyLatencyMap(int num_iter, int src_gpu, int dest_gpu, int dma_id, bool prelaunch) {
    std::cout <<"Collective: " <<  __func__ << (prelaunch ? " (with poll)" : "") << std::endl;

    // P2p uses transfer sizes directly (not divided by numNodes like collectives).
    HSAuint64 p2p_minSize = std::getenv("MIN_SIZE") ? std::stoull(std::getenv("MIN_SIZE")) : 1024;
    HSAuint64 p2p_maxSize = std::getenv("MAX_SIZE") ? std::stoull(std::getenv("MAX_SIZE")) : (1ULL << 30);

    HSAuint64 signalBufSize = 4ULL << 10;
    const HSAuint64 calibration_size = 64ULL << 20;
    int num_phases = prelaunch ? 4 : 3;

    // Calibrate GPU timestamp frequency by timing a DMA copy with CPU wall clock.
    int globalTs_freq_mhz;
    {
        allocateMemP2p(calibration_size, signalBufSize, 1, src_gpu, dest_gpu);
        HSAuint64 value = 1;
        __atomic_store(&signalMem[0][0], &value, __ATOMIC_RELEASE);
        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMAFillDataPacket(
            xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), gpuBufs[src_gpu][0][dest_gpu], 0, calibration_size));
        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMAAtomicPacket(
            xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[0][0]));
        xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();
        while (true) { int64_t v; __atomic_load(&signalMem[0][0], &v, __ATOMIC_ACQUIRE); if (v == 0) break; }

        const HSAuint64 tsBufSize = 4 << 20;
        HsaMemoryBuffer sysBufTsCal(tsBufSize, 0);
        TimeStamp *tsCal = sysBufTsCal.As<TimeStamp*>();
        tsCal = reinterpret_cast<TimeStamp *>ALIGN_UP(tsCal, sizeof(TimeStamp));

        value = 1;
        __atomic_store(&signalMem[0][0], &value, __ATOMIC_RELEASE);
        SDMATimePacket calTs1(&tsCal[0]);
        SDMATimePacket calTs2(&tsCal[1]);
        SDMACopyDataPacket calCopy(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(),
            gpuBufs[dest_gpu][0][src_gpu], gpuBufs[src_gpu][0][dest_gpu], calibration_size);
        SDMAAtomicPacket calSync(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[0][0]);

        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(calTs1);
        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(calCopy);
        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(calTs2);
        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(calSync);

        auto cpuStart = std::chrono::high_resolution_clock::now();
        xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();
        while (true) { int64_t v; __atomic_load(&signalMem[0][0], &v, __ATOMIC_ACQUIRE); if (v == 0) break; }
        auto cpuEnd = std::chrono::high_resolution_clock::now();

        double cpu_ns = std::chrono::duration_cast<std::chrono::duration<double>>(cpuEnd - cpuStart).count() * 1e9;
        HSAuint64 gpu_ticks = tsCal[1].timestamp - tsCal[0].timestamp;
        freeMemP2p(calibration_size, signalBufSize, 1, 1, src_gpu, dest_gpu);

        double freq_mhz_exact = static_cast<double>(gpu_ticks) * 1000.0 / cpu_ns;
        globalTs_freq_mhz = static_cast<int>(freq_mhz_exact + 0.5);
    }

    std::cout << std::dec << std::endl;
    if (g_verbose)
        std::cout << "size,create,enqueue_doorbell,schedule,copy,sync" << std::endl;
    else
        std::cout << "size,control,schedule,copy,sync" << std::endl;

    // Measure hsaKmtGetClockCounters API call overhead.
    double hsakmt_overhead = 0.0;
    {
        const int N = 200;
        std::vector<HSAuint64> deltas;
        for (int i = 0; i < N; i++) {
            HsaClockCounters c1, c2;
            hsaKmtGetClockCounters(gpuNodes[src_gpu], &c1);
            hsaKmtGetClockCounters(gpuNodes[src_gpu], &c2);
            deltas.push_back(c2.SystemClockCounter - c1.SystemClockCounter);
        }
        // Skip first 10 warmup, average rest; convert ticks → ns using SystemClockFrequencyHz
        HsaClockCounters ref; hsaKmtGetClockCounters(gpuNodes[src_gpu], &ref);
        double sys_freq_mhz = static_cast<double>(ref.SystemClockFrequencyHz) / 1e6;
        unsigned long sum = std::accumulate(deltas.begin()+10, deltas.end(), 0UL);
        double call_to_call_ns = (static_cast<double>(sum) / (N-10)) * 1000.0 / sys_freq_mhz;
        hsakmt_overhead = call_to_call_ns / 2.0; // cost of one API call boundary
        if (g_verbose)
            std::cout << "hsaKmtGetClockCounters call-to-call: " << std::fixed << std::setprecision(1)
                      << call_to_call_ns << " ns  boundary overhead: " << hsakmt_overhead << " ns" << std::endl;
    }

    // ts_overhead measured once — it's a hardware constant, not size-dependent.
    double ts_overhead_cached = 0.0;
    bool ts_calibrated = false;

    for (HSAuint64 size = p2p_minSize; size <= p2p_maxSize; size <<= 1) {

        allocateMemP2p(size, 4ULL << 10, num_iter, src_gpu, dest_gpu, num_iter);
        HSAuint64 value = 1;

        for (int iter=0; iter < num_iter; iter++){
            __atomic_store(&signalMem[iter][2], &value, __ATOMIC_RELEASE);
            __atomic_store(&signalMem[iter][3], &value, __ATOMIC_RELEASE);
            xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMAFillDataPacket(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), gpuBufs[src_gpu][iter][dest_gpu], iter, size));
            xgmiSdmaQueue[dest_gpu][dma_id].PlacePacket(SDMAFillDataPacket(xgmiSdmaQueue[dest_gpu][dma_id].GetFamilyId(), gpuBufs[dest_gpu][iter][src_gpu], 0, size));
        }

        xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMAAtomicPacket(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(),&signalMem[num_iter-1][2]));
        xgmiSdmaQueue[dest_gpu][dma_id].PlacePacket(SDMAAtomicPacket(xgmiSdmaQueue[dest_gpu][dma_id].GetFamilyId(),&signalMem[num_iter-1][3]));

        xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();
        xgmiSdmaQueue[dest_gpu][dma_id].SubmitPacket();

        while(true){
            int64_t read_signal_acq1, read_signal_acq2;
            __atomic_load(&signalMem[num_iter-1][2], &read_signal_acq1, __ATOMIC_ACQUIRE);
            __atomic_load(&signalMem[num_iter-1][3], &read_signal_acq2, __ATOMIC_ACQUIRE);
            if (read_signal_acq1 == 0 && read_signal_acq2==0){
                break;
            }
        }

        const HSAuint64 tsBufSize = 4 << 20;
        HsaMemoryBuffer sysBufTs(tsBufSize, 0);
        TimeStamp *tsBuf = sysBufTs.As<TimeStamp*>();
        tsBuf = reinterpret_cast<TimeStamp *>ALIGN_UP(tsBuf, sizeof(TimeStamp));

        std::vector<std::vector<HSAuint64>> copy_times(num_phases);

        // Measure TS packet overhead.
        if (!ts_calibrated) {
            std::vector<HSAuint64> ts_times;
            for (int iter=0; iter < num_iter; iter++){
                TimeStamp *ts = tsBuf + iter * 3;
                HSAuint64 value = 1;
                int signal_id=0;
                __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
                SDMATimePacket ts1(&ts[1]);
                SDMATimePacket ts2(&ts[2]);
                SDMATimePacket ts3(&ts[3]);
                SDMAAtomicPacket sync(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[iter][signal_id]);

                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(ts1);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(ts2);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(ts3);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(sync);
                xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();

                while(true){
                    int64_t read_signal_acq;
                    __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                    if (read_signal_acq == 0) break;
                }
                ts_times.push_back((ts[3].timestamp - ts[1].timestamp)* 1000 / globalTs_freq_mhz);
            }
            unsigned long ts_times_sum=std::accumulate(ts_times.begin()+5, ts_times.end(), 0UL);
            ts_overhead_cached = static_cast<double>(ts_times_sum) / (ts_times.size()-5);
            ts_calibrated = true;
            if (g_verbose) std::cout << "Timestamp packet overhead: " << ts_overhead_cached << " ns" << std::endl;
        }

        std::vector<std::vector<double>> cpu_times(3);

        for (int iter=0; iter < num_iter; iter++){
            std::chrono::high_resolution_clock::time_point t0, t1, t2, t3;
            HSAuint64 value = 1;
            int signal_id=0;
            __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
            __atomic_store(&signalMem[iter][1], &value, __ATOMIC_RELEASE);

            if (prelaunch) {
                HSAuint64 poll_val = 1;
                __atomic_store(&signalMem[iter][4], &poll_val, __ATOMIC_RELEASE);
            }

            t0 = std::chrono::high_resolution_clock::now();

            SDMACopyDataPacket copy(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), gpuBufs[dest_gpu][iter][src_gpu], gpuBufs[src_gpu][iter][dest_gpu], size);
            SDMAAtomicPacket sync(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[iter][signal_id]);
            SDMAPollPacket poll(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[iter][4], 0);

            t1 = std::chrono::high_resolution_clock::now();

            if (prelaunch)
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(poll);
            xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(copy);
            xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(sync);
            xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();

            t2 = std::chrono::high_resolution_clock::now();

            if (prelaunch)
                usleep(100);

            auto t_exec_start = std::chrono::high_resolution_clock::now();

            if (prelaunch) {
                HSAuint64 release_val = 0;
                __atomic_store(&signalMem[iter][4], &release_val, __ATOMIC_RELEASE);
            }

            while(true){
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0){
                    break;
                }
            }

            t3 = std::chrono::high_resolution_clock::now();

            /* validate */
            sdmaQueue[dest_gpu][0].PlaceAndSubmitPacket(SDMACopyDataPacket(sdmaQueue[dest_gpu][0].GetFamilyId(),sysBufs[dest_gpu][0][src_gpu],gpuBufs[dest_gpu][iter][src_gpu], size));
            sdmaQueue[dest_gpu][0].PlaceAndSubmitPacket(SDMAAtomicPacket(sdmaQueue[dest_gpu][0].GetFamilyId(), &signalMem[iter][1]));
            while(true){
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][1], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0){
                    break;
                }
            }

            int end = (int)(size / sizeof(HSAuint64)) - 1;
            for (int j=0; j< end; j++){
                if (sysBufs[dest_gpu][0][src_gpu][j] != (HSAuint32)iter){
                    LOG() << "sysBuf element " << j << " is " << sysBufs[dest_gpu][0][src_gpu][j] << std::endl;
                }
                CHECK_EQ(sysBufs[dest_gpu][0][src_gpu][j], (HSAuint32)iter, "p2p validation failed");
            }

            cpu_times[0].push_back(std::chrono::duration_cast<std::chrono::duration<double> >(t1 - t0).count());
            cpu_times[1].push_back(std::chrono::duration_cast<std::chrono::duration<double> >(t2 - t1).count());
            cpu_times[2].push_back(std::chrono::duration_cast<std::chrono::duration<double> >(t3 - t_exec_start).count());

        }

        auto cpu_avg = [&](int idx) -> double {
            double sum = std::accumulate(cpu_times[idx].begin()+5, cpu_times[idx].end(), 0.0);
            return (sum / (cpu_times[idx].size()-5)) * 1000000000;
        };
        double control_average = cpu_avg(0);
        double enqueue_doorbell_average = cpu_avg(1);
        double exec_cpu_average [[maybe_unused]] = cpu_avg(2); // available with SDMA_VERBOSE

        // GPU-timed breakdown with timestamps around each phase
        for (int iter=0; iter < num_iter; iter++){
            TimeStamp *ts = tsBuf + iter * 32;

            HSAuint64 value = 1;
            int signal_id=0;
            __atomic_store(&signalMem[iter][signal_id], &value, __ATOMIC_RELEASE);
            __atomic_store(&signalMem[iter][6], &value, __ATOMIC_RELEASE);

            SDMACopyDataPacket copy(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), gpuBufs[dest_gpu][iter][src_gpu], gpuBufs[src_gpu][iter][dest_gpu], size);
            SDMAAtomicPacket sync(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[iter][signal_id]);

            if (prelaunch) {
                // ts[0]=CPU clock, TS1 -> POLL -> TS2 -> COPY -> TS3 -> ATOMIC -> TS4
                // phases: fetch(ts1-ts0), poll(ts2-ts1), copy(ts3-ts2), sync(ts4-ts3)
                HSAuint64 poll_val = 1;
                __atomic_store(&signalMem[iter][4], &poll_val, __ATOMIC_RELEASE);

                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[1]));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMAPollPacket(xgmiSdmaQueue[src_gpu][dma_id].GetFamilyId(), &signalMem[iter][4], 0));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[2]));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(copy);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[3]));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(sync);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[4]));

                hsaKmtGetClockCounters(gpuNodes[src_gpu], reinterpret_cast<HsaClockCounters*>(&ts[0]));

                xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();

                // CPU triggers the poll signal after doorbell
                HSAuint64 release_val = 0;
                __atomic_store(&signalMem[iter][4], &release_val, __ATOMIC_RELEASE);
            } else {
                // ts[0]=CPU clock, TS1 -> COPY -> TS2 -> ATOMIC -> TS3
                // phases: fetch(ts1-ts0), copy(ts2-ts1), sync(ts3-ts2)
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[1]));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(copy);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[2]));
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(sync);
                xgmiSdmaQueue[src_gpu][dma_id].PlacePacket(SDMATimePacket(&ts[3]));

                hsaKmtGetClockCounters(gpuNodes[src_gpu], reinterpret_cast<HsaClockCounters*>(&ts[0]));

                xgmiSdmaQueue[src_gpu][dma_id].SubmitPacket();
            }

            while(true){
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][signal_id], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0){
                    break;
                }
            }

            /* validate */
            sdmaQueue[dest_gpu][0].PlaceAndSubmitPacket(SDMACopyDataPacket(sdmaQueue[dest_gpu][0].GetFamilyId(),sysBufs[dest_gpu][0][src_gpu],gpuBufs[dest_gpu][iter][src_gpu], size));
            sdmaQueue[dest_gpu][0].PlaceAndSubmitPacket(SDMAAtomicPacket(sdmaQueue[dest_gpu][0].GetFamilyId(), &signalMem[iter][6]));
            while(true){
                int64_t read_signal_acq;
                __atomic_load(&signalMem[iter][6], &read_signal_acq, __ATOMIC_ACQUIRE);
                if (read_signal_acq == 0){
                    break;
                }
            }

            int end = (int)(size / sizeof(HSAuint64)) - 1;
            for (int j=0; j< end; j++){
                if (sysBufs[dest_gpu][0][src_gpu][j] != (HSAuint32)iter)
                    LOG() << "sysBuf element " << j << " is " << sysBufs[dest_gpu][0][src_gpu][j] << std::endl;
                CHECK_EQ(sysBufs[dest_gpu][0][src_gpu][j], (HSAuint32)iter, "p2p validation failed");
            }

            for(int k=0;k<num_phases;k++){
                copy_times[k].push_back((ts[k+1].timestamp - ts[k].timestamp)* 1000 / globalTs_freq_mhz);
            }

        }

        // Subtract TS packet and API call overhead from phase averages.
        double ts_overhead = ts_overhead_cached / 2.0;
        std::vector<double> phase_avg(num_phases);
        for (int k = 0; k < num_phases; k++) {
            unsigned long sum = std::accumulate(copy_times[k].begin()+5, copy_times[k].end(), 0UL);
            phase_avg[k] = static_cast<double>(sum) / (copy_times[k].size()-5) - ts_overhead;
        }
        // Fetch (phase[0]): additionally subtract hsaKmt API call boundary.
        phase_avg[0] -= hsakmt_overhead;

        // With PRELAUNCH: control and schedule are off the critical path.
        double control_total = control_average + enqueue_doorbell_average;
        if (g_verbose) {
            std::cout << std::dec << size
                      << "," << control_average       // create
                      << "," << enqueue_doorbell_average  // enqueue+doorbell
                      << "," << phase_avg[0]          // schedule
                      << "," << phase_avg[prelaunch ? 2 : 1]  // copy
                      << "," << phase_avg[prelaunch ? 3 : 2]  // sync
                      << std::endl;
        } else {
            std::cout << std::dec << size
                      << "," << control_total          // control
                      << "," << phase_avg[0]           // schedule
                      << "," << phase_avg[prelaunch ? 2 : 1]  // copy
                      << "," << phase_avg[prelaunch ? 3 : 2]  // sync
                      << std::endl;
        }

        freeMemP2p(size, signalBufSize, num_iter, num_iter, src_gpu, dest_gpu);
    }

    destroyQueues();
}


