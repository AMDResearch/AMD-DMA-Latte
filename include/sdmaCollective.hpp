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


#include "sdmaBase.hpp"
#include "XgmiOptimizedSDMAQueue.hpp"
#include <chrono>
#include <map>


extern bool g_verbose;  // set from SDMA_VERBOSE env var at startup

// Describes one sDMA command issued by a source GPU:
//   engine_id — which xGMI sDMA engine to use
//   dests     — destination GPU indices (1 for pcpy/swap, 2 for bcst, N for b2b)
struct sdmaCommand {
    int engine_id;
    std::vector<int> dests;
};

class sdmaCollective : public sdmaBase {
 public:
    sdmaCollective() {}
    ~sdmaCollective() {}

    virtual void SetUp();   ///< Initialises HSA runtime and discovers GPU topology.
    virtual void TearDown(); ///< Shuts down HSA runtime and releases resources.

    // general functions
    void initialize(HSAuint64 min_size, HSAuint64 max_size, std::vector<int> gpus, std::ifstream* engineMapFile, bool sdmaQueueSize); ///< Creates SDMA queues for all GPUs.
    void getPreferredEngines(bool sdma_to_dest); ///< Populates preferredEngines from the engine map (or round-robin if no map).

    // Per-command packet helpers — used by all collectives
    void placeSyncPacket(XgmiOptimizedSDMAQueue& queue, HSAuint64* signal, int sync);  ///< Appends a completion packet (fence or atomic) to the queue.
    void placePollPacket(XgmiOptimizedSDMAQueue& queue, int num_cmds, int iter,
                         int gpu, int cmd_idx, bool poll, bool multi_poll);             ///< Appends a GPU-side poll packet when PRELAUNCH is set.

    // Per-iteration timing helpers 
    void initSignals(int num_cmds, int iter, bool poll, bool multi_poll);  ///< Zeroes signal memory and sets up trigger values for one iteration.
    void triggerPoll(int num_cmds, int iter, bool poll, bool multi_poll,
                     float trigger_time, int& t_index,
                     std::vector<std::chrono::high_resolution_clock::time_point>& times); ///< Writes poll signals to start the queued DMA commands and records the trigger timestamp.
    // done_target: completions to wait for; defaults to num_cmds * numNodes
    void spinWait(int num_cmds, int iter, std::vector<int>& dest_done,
                  std::vector<std::chrono::high_resolution_clock::time_point>& times,
                  int& t_index, int done_target = -1); ///< Spins on signal memory until all expected completions arrive; records completion timestamp.

    // mem allocater: allocates buffers, makes all peer accessible. data/sys buf size represents chunk size
    void allocateMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter); ///< Allocates GPU and system buffers; maps all to peer-accessible address space.
    void allocateMemP2p(HSAuint64 size, HSAuint64 signalBufSize, int num_iter, int src, int dst, int sig_iters = 0); ///< P2p-only allocation: direct size (no ×numNodes), only src and dst GPU buffers.
    void fillMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter);     ///< Fills GPU buffers with deterministic patterns for validation.
    void validateMem(std::string collective, HSAuint64 dataBufSize, bool inplace, int num_iter); ///< Checks GPU output buffers against expected patterns.
    void logTimes(int total_transfers, std::vector<std::chrono::high_resolution_clock::time_point>& times, bool poll); ///< Records per-transfer timestamps for one iteration.
    void finalizeTime(HSAuint64 dataBufSize, int total_transfers, bool poll, int num_iter); ///< Computes and prints latency/bandwidth summary across all iterations.
    void freeMem(HSAuint64 dataBufSize, HSAuint64 signalBufSize, int num_iter); ///< Unmaps and frees all GPU and system buffers.
    void freeMemP2p(HSAuint64 size, HSAuint64 signalBufSize, int sig_iters, int buf_iters, int src, int dst); ///< Frees only src/dst GPU buffers allocated by allocateMemP2p.
    void destroyQueues(); ///< Destroys all SDMA queues.

    // collectives
    void P2pCopyLatencyMap(int num_iter, int src_gpu, int dest_gpu, int dma_id, bool prelaunch = false); ///< Measures fetch/copy/sync latency breakdown for a single GPU-to-GPU DMA copy.
    void AllgatherNwayB2b(int num_iter, std::vector<int> dma, int ways, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);  ///< All-gather: back-to-back copies, N destinations per engine.
    void AllgatherPcpy(int num_iter,  int sync, bool poll, bool multi_poll, float trigger_time, std::vector<int> gpus, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);             ///< All-gather: one engine per destination GPU (parallel copy).
    void AllgatherBcst(int num_iter,  int sync, bool poll, bool multi_poll, float trigger_time, std::vector<int> gpus, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);             ///< All-gather: broadcast packet writing to 2 destinations per command.
    void AlltoallPcpy(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);                                      ///< All-to-all: one engine per (src, dst) pair (parallel copy).
    void AlltoallNwayB2b(int num_iter, std::vector<int> dma, int ways, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);  ///< All-to-all: back-to-back copies on fewer engines.
    void AlltoallSwap(int num_iter, int sync, bool poll, bool multi_poll, float trigger_time, std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& latency_map);                                      ///< All-to-all: in-place bidirectional swap in a single DMA command.
    
private:
    HSAuint64 minSize;
    HSAuint64 maxSize;
    std::vector<HSAint32> allGpuNodes;
    int numTotalNodes;
    std::vector<int> gpus;
    std::vector<HSAint32> gpuNodes;
    int numNodes;
    std::vector<std::vector<XgmiOptimizedSDMAQueue>> xgmiSdmaQueue;
    std::vector<std::vector<SDMAQueue>> sdmaQueue;
    int numTotalSdma;   // set at runtime from node properties (NumSdmaEngines + NumSdmaXgmiEngines)
    int numCpuSdma;     // set at runtime from node properties (NumSdmaEngines)
    HSAuint64 xgmiQueueSize; // actual xGMI queue size in bytes (set in initialize())
    std::ifstream* engineMap;
    std::map<std::pair<int, int>, uint32_t> preferredEngines;
    std::map<int, int> hwToLocal; // hardware GPU index → local collective index
    std::vector<std::vector<void*>> gpuMem;
    std::vector<std::vector<void*>> gpuOutMem;
    std::vector<std::vector<std::vector<void*>>> gpuBufs;
    std::vector<std::vector<std::vector<void*>>> gpuOutBufs;
    std::vector<std::vector<HSAuint32*>> sysMem;
    std::vector<std::vector<std::vector<HSAuint32*>>> sysBufs;
    std::vector<HSAuint64*> signalMem;
    std::vector<double>  controlTime;
    std::vector<double>  scheduleCopyTime;
    std::vector<std::vector<double>> cpu_times;
    std::map<HSAuint64, double> sizeTimeMap; // timing map

};
