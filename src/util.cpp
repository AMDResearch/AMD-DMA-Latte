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

#include "util.hpp"
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <algorithm>
#include <vector>


bool WaitOnValue(const volatile unsigned int *buf, unsigned int value, unsigned int timeOut) {
    while (timeOut > 0 && *buf != value) {
        usleep(1000); // 1ms

        if (timeOut != HSA_EVENTTIMEOUT_INFINITE)
            timeOut--;
    }

    return *buf == value;
}

void SplitU64(const HSAuint64 value, unsigned int& rLoPart, unsigned int& rHiPart) {
    rLoPart = static_cast<unsigned int>(value);
    rHiPart = static_cast<unsigned int>(value >> 32);
}


HSAKMT_STATUS CreateQueueTypeEvent(
    bool                ManualReset,            // IN
    bool                IsSignaled,             // IN
    unsigned int        NodeId,                 // IN
    HsaEvent**          Event                   // OUT
    ) {
    HsaEventDescriptor Descriptor;

// TODO: Create per-OS header with this sort of definitions
#ifdef _WIN32
    Descriptor.EventType = HSA_EVENTTYPE_QUEUE_EVENT;
#else
    Descriptor.EventType = HSA_EVENTTYPE_SIGNAL;
#endif
    Descriptor.SyncVar.SyncVar.UserData = (void*)0xABCDABCD;
    Descriptor.NodeId = NodeId;

    return hsaKmtCreateEvent(&Descriptor, ManualReset, IsSignaled, Event);
}

static bool hsakmt_is_dgpu_dev = false;

bool hsakmt_is_dgpu() {
    return hsakmt_is_dgpu_dev;
}

unsigned int FamilyIdFromNode(const HsaNodeProperties *props) {
    unsigned int familyId = FAMILY_UNKNOWN;

    switch (props->EngineId.ui32.Major) {
    case 7:
        if (props->EngineId.ui32.Minor == 0) {
            if (props->EngineId.ui32.Stepping == 0)
                familyId = FAMILY_KV;
            else
                familyId = FAMILY_CI;
        }
        break;
    case 8:
        familyId = FAMILY_VI;
        if (props->EngineId.ui32.Stepping == 1)
            familyId = FAMILY_CZ;
        break;
    case 9:
        familyId = FAMILY_AI;
        if (props->EngineId.ui32.Minor == 4)
            familyId = FAMILY_AV;
        else if (props->EngineId.ui32.Stepping == 2)
            familyId = FAMILY_RV;
        else if (props->EngineId.ui32.Stepping == 8)
            familyId = FAMILY_AR;
        else if (props->EngineId.ui32.Stepping == 10)
            familyId = FAMILY_AL;
        break;
    case 10:
        familyId = FAMILY_NV;
        break;
    case 11:
        familyId = FAMILY_GFX11;
        break;
    case 12:
        familyId = FAMILY_GFX12;
	break;
    }

    if (props->NumCPUCores && props->NumFComputeCores)
        hsakmt_is_dgpu_dev = false;
    else
        hsakmt_is_dgpu_dev = true;

    return familyId;
}

void GetHwQueueInfo(const HsaNodeProperties *props,
                 unsigned int *p_num_cp_queues,
                 unsigned int *p_num_sdma_engines,
                 unsigned int *p_num_sdma_xgmi_engines,
                 unsigned int *p_num_sdma_queues_per_engine) {
    if (p_num_sdma_engines)
        *p_num_sdma_engines = props->NumSdmaEngines;

    if (p_num_sdma_xgmi_engines)
        *p_num_sdma_xgmi_engines = props->NumSdmaXgmiEngines;

    if (p_num_sdma_queues_per_engine)
        *p_num_sdma_queues_per_engine = props->NumSdmaQueuesPerEngine;

    if (p_num_cp_queues)
        *p_num_cp_queues = props->NumCpQueues;
}


// Returns the gfx arch string, e.g. "gfx942" (MI300X) or "gfx90a" (MI210).
// Stepping >= 10 is rendered as a hex letter to match ROCm naming conventions.
std::string GfxArchString(const HsaNodeProperties *props) {
    char buf[16];
    snprintf(buf, sizeof(buf), "gfx%u%u%x",
             props->EngineId.ui32.Major,
             props->EngineId.ui32.Minor,
             props->EngineId.ui32.Stepping);
    return buf;
}

HsaMemoryBuffer::HsaMemoryBuffer(HSAuint64 size, unsigned int node, bool zero, bool isLocal, bool isExec,
                                 bool isScratch, bool isReadOnly, bool isUncached, bool NonPaged)
    :m_Size(size),
    m_pUser(NULL),
    m_pBuf(NULL),
    m_Local(isLocal),
    m_Node(node) {
    m_Flags.Value = 0;

    HsaMemMapFlags mapFlags = {0};
    bool map_specific_gpu = (node && !isScratch);

    if (isScratch) {
        m_Flags.ui32.Scratch = 1;
        m_Flags.ui32.HostAccess = 1;
    } else {
        m_Flags.ui32.PageSize = HSA_PAGE_SIZE_4KB;

        if (isLocal) {
            m_Flags.ui32.HostAccess = 0;
            m_Flags.ui32.NonPaged = 1;
            m_Flags.ui32.CoarseGrain = 1;
            EXPECT_EQ(isUncached, 0, "Uncached flag is relevant only for system or host memory");
        } else {
            m_Flags.ui32.HostAccess = 1;
            m_Flags.ui32.NonPaged = NonPaged ? 1 : 0;
            m_Flags.ui32.CoarseGrain = 0;
            m_Flags.ui32.NoNUMABind = 1;
            m_Flags.ui32.Uncached = isUncached;
        }

        if (isExec)
            m_Flags.ui32.ExecuteAccess = 1;
    }
    if (isReadOnly)
        m_Flags.ui32.ReadOnly = 1;

    if (zero)
        EXPECT_EQ(m_Flags.ui32.HostAccess, 1, "Not Equal");

    EXPECT_SUCCESS(hsaKmtAllocMemory(m_Node, m_Size, m_Flags, &m_pBuf), "Failed!");
    if (hsakmt_is_dgpu()) {
        if (map_specific_gpu)
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPUNodes(m_pBuf, m_Size, NULL, mapFlags, 1, &m_Node), "Failed!");
        else
            EXPECT_SUCCESS(hsaKmtMapMemoryToGPU(m_pBuf, m_Size, NULL), "Failed!");
        m_MappedNodes = 1 << m_Node;
    }

    if (zero && !isLocal)
        memset(m_pBuf, 0, m_Size);
}

unsigned int HsaMemoryBuffer::Size() {
    return m_Size;
}

void HsaMemoryBuffer::UnmapAllNodes() {
    unsigned int *Arr, size, i, j;
    int bit;

    size = 0;
    for (i = 0; i < 8; i++) {
        bit = 1 << i;
        if (m_MappedNodes & bit)
            size++;
    }

    Arr = (unsigned int *)malloc(sizeof(unsigned int) * size);
    if (!Arr)
        return;

    for (i = 0, j =0; i < 8; i++) {
        bit = 1 << i;
        if (m_MappedNodes & bit)
            Arr[j++] = i;
    }

    /*
     * TODO: When thunk is updated, use hsaKmtRegisterToNodes. Then nodes will be used
     */
    hsaKmtUnmapMemoryToGPU(m_pBuf);

    m_MappedNodes = 0;

    free(Arr);
}

HsaMemoryBuffer::~HsaMemoryBuffer() {
    if (m_pUser != NULL) {
        hsaKmtUnmapMemoryToGPU(m_pUser);
        hsaKmtDeregisterMemory(m_pUser);
    } else if (m_pBuf != NULL) {
        if (hsakmt_is_dgpu()) {
            if (m_MappedNodes) {
                hsaKmtUnmapMemoryToGPU(m_pBuf);
            }
        }
        hsaKmtFreeMemory(m_pBuf, m_Size);
    }
    m_pBuf = NULL;
}

HsaNodeInfo::HsaNodeInfo() {
}

/* Init - Get and store information about all the HSA nodes from the Thunk Library.
 * @NumOfNodes - Number to system nodes returned by hsaKmtAcquireSystemProperties
 * @Return - false: if no node information is available
 */
bool HsaNodeInfo::Init(int NumOfNodes) {
    HsaNodeProperties *nodeProperties;
    _HSAKMT_STATUS status;
    bool ret = false;

    for (int i = 0; i < NumOfNodes; i++) {
        nodeProperties = new HsaNodeProperties();

        status = hsaKmtGetNodeProperties(i, nodeProperties);
        /* This is not a fatal test (not using assert), since even when it fails for one node
         * we want to get information regarding others.
         */
        EXPECT_SUCCESS(status, "Failed!");
        //std::cout << "Node index: " << i << "hsaKmtGetNodeProperties returned status " << status;

        if (status == HSAKMT_STATUS_SUCCESS) {
            m_HsaNodeProps.push_back(nodeProperties);
            ret = true;  // Return true if atleast one information is available

            if (nodeProperties->NumFComputeCores)
                m_NodesWithGPU.push_back(i);
        } else {
            delete nodeProperties;
        }
    }

    return ret;
}

HsaNodeInfo::~HsaNodeInfo() {

    for (unsigned int i = 0; i < m_HsaNodeProps.size(); i++)
        delete m_HsaNodeProps.at(i);

    m_HsaNodeProps.clear();
}

const std::vector<int>& HsaNodeInfo::GetNodesWithGPU() const {
    return m_NodesWithGPU;
}

const HsaNodeProperties* HsaNodeInfo::GetNodeProperties(int NodeNum) const {
    return m_HsaNodeProps.at(NodeNum);
}

const HsaNodeProperties* HsaNodeInfo::HsaDefaultGPUNodeProperties() const {
    int NodeNum = HsaDefaultGPUNode();
    if (NodeNum < 0)
        return NULL;
    return GetNodeProperties(NodeNum);
}

const int HsaNodeInfo::HsaDefaultGPUNode() const {
    if (m_NodesWithGPU.size() == 0)
        return -1;
/*
    if (g_TestNodeId >= 0) {
        // Check if this is a valid Id, if so use this else use first available
        for (unsigned int i = 0; i < m_NodesWithGPU.size(); i++) {
            if (g_TestNodeId == m_NodesWithGPU.at(i))
                return g_TestNodeId;
        }
    }
*/
    return m_NodesWithGPU.at(0);
}

