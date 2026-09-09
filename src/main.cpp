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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <iostream>
#include <functional>
#include <sstream>
#include "sdmaCollective.hpp"
#include <map>
#include <cmath>
#include <iomanip>
#include <fstream>

class sdmaBase *g_baseTest;
bool g_verbose;

//average across all pairs
void average(std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>>& data){
    std::map<HSAuint64, double> sum;
    std::map<HSAuint64, int> count;

    for (const auto& level1 : data) {
        for (const auto& level2 : level1.second) {
            for (const auto& level3 : level2.second) {
                sum[level3.first] += level3.second;  
                count[level3.first]++;               
            }
        }
    }

    std::map<HSAuint64, double> averages;
    for (const auto& entry : sum) {
        averages[entry.first] = entry.second / count[entry.first];
    }

    std::cout << "Averaged values:\n";
    std::cout << std::fixed << "collective size(bytes),latency(ns),link bandwidth(GB/s)" << std::endl;
    for (const auto& entry : averages) {
        double bw = (static_cast<double>(entry.first)/(1024*1024*1024))/(entry.second/(1000000000));
        std::cout << std::fixed << std::setprecision(2) << entry.first << "," << entry.second << "," << bw << std::endl;
    }
}

int main(){

    g_verbose = std::getenv("SDMA_VERBOSE");

    // Collective selection
    std::string collectiveType = std::getenv("COLLECTIVE_TYPE") ? std::getenv("COLLECTIVE_TYPE") : "ag"; // ag, aa, p2p
    std::string variant        = std::getenv("VARIANT")         ? std::getenv("VARIANT")         : "pcpy"; // pcpy, bcst, swap, b2b

    // Transfer parameters
    const char* sdmaMap   = std::getenv("SDMA_MAP");   // engine affinity map (see scripts/gen_sdma_map_sysfs.py)
    // GPU_LIST: comma-separated GPU indices, e.g. "0,1,4,5" for a 4-GPU collective on an 8-GPU system.
    // If not set, uses NUM_GPUS sequential GPUs starting from 0.
    int numGpus    = std::getenv("NUM_GPUS")   ? std::stoi(std::getenv("NUM_GPUS"))   : 8;
    long minSize   = std::getenv("MIN_SIZE")   ? std::stol(std::getenv("MIN_SIZE"))   : 128;    // min collective size (bytes)
    long maxSize   = std::getenv("MAX_SIZE")   ? std::stol(std::getenv("MAX_SIZE"))   : 1<<30;  // max collective size (bytes)
    int numIter    = std::getenv("NUM_ITER")   ? std::stoi(std::getenv("NUM_ITER"))   : 20;     // iterations (first 5 warmup)
    int syncType   = std::getenv("SYNC_TYPE")  ? std::stoi(std::getenv("SYNC_TYPE"))  : 2;      // 0=32b fence, 1=2x32b fence, 2=64b atomic
    bool prelaunch  = std::getenv("PRELAUNCH") ? std::stoi(std::getenv("PRELAUNCH")) : false; // queue commands ahead of time; trigger with a memory write
    bool multiSignal  = std::getenv("MULTI_SIGNAL")         ? std::stoi(std::getenv("MULTI_SIGNAL"))         : false; // one trigger signal per engine (use with PRELAUNCH=1)
    bool sdmaQueueSize = std::getenv("SDMA_QUEUE_SIZE")     ? std::stoi(std::getenv("SDMA_QUEUE_SIZE"))      : true;  // 0=4KB, 1=2MB (default: 2MB)

    sdmaCollective collective;
    std::map<std::string, std::map<std::string, std::map<HSAuint64,double>>> latencies;

    // SDMA_MAP provides per-(src,dest) engine affinity for best performance on MI3xx.
    // On MI210 engines have no destination affinity; SDMA_MAP is ignored and round-robin
    // assignment is used. On MI3xx, omitting SDMA_MAP also falls back to round-robin
    // with a warning. See scripts/gen_sdma_map_sysfs.py to generate the map for your machine.
    std::ifstream engineMap;
    if (sdmaMap) {
        engineMap.open(sdmaMap);
        if (!engineMap.is_open()) {
            std::cerr << "[ERROR] Could not open SDMA_MAP file: " << sdmaMap << "\n";
            return 1;
        }
    }

    // Initialises HSA runtime and discovers GPU topology (agents, SDMA engine counts).
    collective.SetUp();

    float trigger_time = 0.01;
    std::vector<int> all_gpus;
    const char* gpuList = std::getenv("GPU_LIST");
    if (gpuList) {
        std::stringstream ss(gpuList);
        std::string token;
        while (std::getline(ss, token, ','))
            all_gpus.push_back(std::stoi(token));
    } else {
        for (int j = 0; j < numGpus; j++)
            all_gpus.push_back(j);
    }
    collective.initialize(minSize, maxSize, all_gpus, sdmaMap ? &engineMap : nullptr, sdmaQueueSize);

    // Dispatch table: key = "collective_variant"
    // VARIANT: pcpy (parallel copy), bcst (broadcast), swap, b2b (back-to-back)
    // B2B_DMA: comma-separated xGMI engine IDs to use for b2b (e.g. "8,10"); defaults to engine 8.
    // B2B_WAYS: destinations per engine (back-to-back copies); higher = fewer engines, more sequential work.
    std::vector<int> b2b_dma = {8};
    if (const char* b2bDmaEnv = std::getenv("B2B_DMA")) {
        b2b_dma.clear();
        std::stringstream ss(b2bDmaEnv);
        std::string token;
        while (std::getline(ss, token, ','))
            b2b_dma.push_back(std::stoi(token));
    }
    int b2b_ways = std::getenv("B2B_WAYS") ? std::stoi(std::getenv("B2B_WAYS")) : 7;

    std::map<std::string, std::function<void()>> dispatch = {
        {"ag_pcpy", [&]{ collective.getPreferredEngines(false);
                         collective.AllgatherPcpy(numIter,syncType,prelaunch,multiSignal,trigger_time,all_gpus,latencies); }},
        {"ag_bcst", [&]{ collective.getPreferredEngines(true);
                         collective.AllgatherBcst(numIter,syncType,prelaunch,multiSignal,trigger_time,all_gpus,latencies); }},
        {"ag_b2b",  [&]{ collective.getPreferredEngines(false);
                         collective.AllgatherNwayB2b(numIter,b2b_dma,b2b_ways,syncType,prelaunch,multiSignal,trigger_time,latencies); }},
        {"aa_pcpy", [&]{ collective.getPreferredEngines(false);
                         collective.AlltoallPcpy(numIter,syncType,prelaunch,multiSignal,trigger_time,latencies); }},
        {"aa_swap", [&]{ collective.getPreferredEngines(false);
                         collective.AlltoallSwap(numIter,syncType,prelaunch,multiSignal,trigger_time,latencies); }},
        {"aa_b2b",  [&]{ collective.getPreferredEngines(false);
                         collective.AlltoallNwayB2b(numIter,b2b_dma,b2b_ways,syncType,prelaunch,multiSignal,trigger_time,latencies); }},
        {"p2p_latency", [&]{ collective.getPreferredEngines(true);
                         collective.P2pCopyLatencyMap(numIter, 0, 1, 4, prelaunch); }},
    };


    std::string key = collectiveType + "_" + variant;
    auto it = dispatch.find(key);
    if (it == dispatch.end()) {
        std::cerr << "[ERROR] Unknown COLLECTIVE_TYPE/VARIANT: " << collectiveType << " / " << variant << "\n"
                  << "  COLLECTIVE_TYPE: ag, aa, p2p\n"
                  << "  VARIANT:         pcpy, bcst (ag only), swap (aa only), b2b\n"
                  << "                   latency (p2p only)\n";
        collective.TearDown();
        return 1;
    }
    it->second();

    // Print latency summary when populated (e.g. p2p latency breakdown).
    // ag/aa collectives print their own formatted output and do not populate this.
    if (!latencies.empty()) {
        std::cout << std::endl;
        average(latencies);
        for (const auto& level1 : latencies) {
            for (const auto& level2 : level1.second) {
                std::cout << level1.first << " to " << level2.first << std::endl;
                for (const auto& level3 : level2.second) {
                    double bw = (static_cast<double>(level3.first)/(1024*1024*1024))/(level3.second/(1000000000));
                    std::cout << std::fixed << std::setprecision(2) << level3.first << "," << level3.second << "," << bw <<  std::endl;
                }
            }
        }
    }

    collective.TearDown();
    return 0;
}
