/*
 * Copyright (C) 2014-2026 Advanced Micro Devices, Inc. All Rights Reserved.
 * Derived from ROCm/rocm-systems kfdtest, adapted for AMD DMA-Latte.
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
#ifndef __KFD__TEST__UTIL__H__
#define __KFD__TEST__UTIL__H__

#include <vector>
#include "KFDTestFlags.hpp"
#include "hsakmt/hsakmt.h"

#ifdef HSAKMT_NO_MAP_FLAGS
typedef HsaMemFlags HsaMemMapFlags;
#endif
#include <iostream>
#include <cstdlib> // For exit()
#include <sstream>
#include <cstring>
#include <iomanip>

// Utility function to check HSAKMT status and log errors
#define CHECK_HSAKMT_SUCCESS(call, msg) \
    do { \
        if ((call) != HSAKMT_STATUS_SUCCESS) { \
            std::cerr << "ERROR: " << msg << " (File: " << __FILE__ << ", Line: " << __LINE__ << ")" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

// Macro to check equality
#define CHECK_EQ(val1, val2, msg) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << "ERROR: " << msg << " | Expected: " << (val2) << ", Got: " << (val1) \
                      << " (File: " << __FILE__ << ", Line: " << __LINE__ << ")" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

// Macro to check if condition is true
#define CHECK_TRUE(condition, msg) \
    do { \
        if (!(condition)) { \
            std::cerr << "ERROR: " << msg << " | Condition failed: " #condition \
                      << " (File: " << __FILE__ << ", Line: " << __LINE__ << ")" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

// Macro to check if first value is greater than second
#define CHECK_GT(val1, val2, msg) \
    do { \
        if (!((val1) > (val2))) { \
            std::cerr << "ERROR: " << msg << " | Expected: " << (val1) << " > " << (val2) \
                      << " (File: " << __FILE__ << ", Line: " << __LINE__ << ")" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#define CHECK_GE(actual, expected, msg) \
    do { \
        if (!((actual) >= (expected))) { \
            std::cerr << "ERROR: " << msg << " (Expected " #actual " >= " #expected \
                      << " but got " << actual << " vs " << expected << ") " \
                      << "[File: " << __FILE__ << ", Line: " << __LINE__ << "]" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

// Macro to check if pointer is not null
#define CHECK_NOTNULL(ptr, msg) \
    do { \
        if ((ptr) == nullptr) { \
            std::cerr << "ERROR: " << msg << " | Got nullptr" \
                      << " (File: " << __FILE__ << ", Line: " << __LINE__ << ")" << std::endl; \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

#define EXPECT_SUCCESS(expr, msg) \
    do { \
        if ((expr) != HSAKMT_STATUS_SUCCESS) { \
            std::cerr << "EXPECT FAILURE: " << msg << " (" #expr " failed!) " \
                      << "[File: " << __FILE__ << ", Line: " << __LINE__ << "]" << std::endl; \
        } \
    } while (0)

#define EXPECT_EQ(actual, expected, msg) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "EXPECT FAILURE: " << msg << " (Expected " #actual " == " #expected \
                      << " but got " << actual << " vs " << expected << ") " \
                      << "[File: " << __FILE__ << ", Line: " << __LINE__ << "]" << std::endl; \
        } \
    } while (0)

#define EXPECT_NE(actual, expected, msg) \
    do { \
        if ((actual) == (expected)) { \
            std::cerr << "EXPECT FAILURE: " << msg << " (Expected " #actual " != " #expected \
                      << " but got " << actual << " == " << expected << ") " \
                      << "[File: " << __FILE__ << ", Line: " << __LINE__ << "]" << std::endl; \
        } \
    } while (0)

#define EXPECT_NOTNULL(ptr, msg) \
    do { \
        if ((ptr) == nullptr) { \
            std::cerr << "EXPECT FAILURE: " << msg << " (Expected " #ptr " to be non-null!) " \
                      << "[File: " << __FILE__ << ", Line: " << __LINE__ << "]" << std::endl; \
        } \
    } while (0)


class Logger : public std::ostringstream {
public:
    ~Logger() {
        std::cout << this->str() << std::endl; // Print log messages when destroyed
    }
};

// Helper function to return an lvalue reference
inline Logger& GetLoggerInstance() {
    static Logger instance;
    return instance;
}

// Macro using the helper function
#define LOG() GetLoggerInstance()

#define WARN() std::cerr << "[WARN] "

#define KFD_TEST_DEFAULT_TIMEOUT 10000

#define ALIGN_UP(x, align) (((uint64_t)(x) + (align) - 1) & ~(uint64_t)((align)-1))



// @brief: waits until the value is written to the buffer or until time out if received through args
bool WaitOnValue(const volatile unsigned int *buf, unsigned int value, unsigned int timeOut = KFD_TEST_DEFAULT_TIMEOUT);

void SplitU64(const HSAuint64 value, unsigned int& rLoPart, unsigned int& rHiPart);

HSAKMT_STATUS CreateQueueTypeEvent(bool ManualReset, bool IsSignaled, unsigned int NodeId, HsaEvent** Event);

bool hsakmt_is_dgpu();
unsigned int FamilyIdFromNode(const HsaNodeProperties *props);
std::string GfxArchString(const HsaNodeProperties *props);

void GetHwQueueInfo(const HsaNodeProperties *props,
                 unsigned int *p_num_cp_queues,
                 unsigned int *p_num_sdma_engines,
                 unsigned int *p_num_sdma_xgmi_engines,
                 unsigned int *p_num_sdma_queues_per_engine);

typedef struct {
        HSAuint64 timestamp;
        HSAuint64 timeConsumption;
        HSAuint64 timeBegin;
        HSAuint64 timeEnd;
} TimeStamp;

class HsaMemoryBuffer {
 public:
    HsaMemoryBuffer(HSAuint64 size, unsigned int node, bool zero = true, bool isLocal = false,
                    bool isExec = false, bool isScratch = false, bool isReadOnly = false, bool isUncached = false, bool NonPaged = false);
    ~HsaMemoryBuffer();

    template<typename RetType>
    RetType As() {
        return reinterpret_cast<RetType>(m_pBuf);
    }

    template<typename RetType>
    const RetType As() const {
        return reinterpret_cast<const RetType>(m_pBuf);
    }

    unsigned int Size();

 private:
    // Disable copy
    HsaMemoryBuffer(const HsaMemoryBuffer&);
    const HsaMemoryBuffer& operator=(const HsaMemoryBuffer&);

    void UnmapAllNodes();

 private:
    HsaMemFlags m_Flags;
    HSAuint64 m_Size;
    void* m_pUser;
    void* m_pBuf;
    bool m_Local;
    unsigned int m_Node;
    HSAuint64 m_MappedNodes;
};


// @class HsaNodeInfo - Gather and store all HSA node information from Thunk.
class HsaNodeInfo {
    // List containing HsaNodeProperties of all Nodes available
    std::vector<HsaNodeProperties*> m_HsaNodeProps;

    // List of HSA Nodes that contain a GPU. This includes both APU and dGPU
    std::vector<int> m_NodesWithGPU;

 public:
    HsaNodeInfo();
    ~HsaNodeInfo();

    bool Init(int NumOfNodes);

    /* This function should be deprecated soon. This for transistion purpose only
     * Currently, KfdTest is designed to test only ONE node. This function acts
     * as transition.
     */
    const HsaNodeProperties* HsaDefaultGPUNodeProperties() const;
    const int HsaDefaultGPUNode() const;

    /* TODO: Use the following two functions to support multi-GPU.
     * const std::vector<int>& GpuNodes = GetNodesWithGPU()
     * for (..GpuNodes.size()..) GetNodeProperties(GpuNodes.at(i))
     */
    const std::vector<int>& GetNodesWithGPU() const;

    // @param node index of the node we are looking at
    // @param nodeProperties HsaNodeProperties returned
    const HsaNodeProperties* GetNodeProperties(int NodeNum) const;

};

#endif  // __KFD__TEST__UTIL__H__
