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

#ifndef __KFD_BASE_QUEUE__H__
#define __KFD_BASE_QUEUE__H__

#include <vector>
#include "util.hpp"
#include "BasePacket.hpp"

#ifndef PAGE_SIZE
#define PAGE_SIZE (1<<12)
#endif

class BaseQueue {
 public:
    static const unsigned int DEFAULT_QUEUE_SIZE       = PAGE_SIZE;
    static const HSA_QUEUE_PRIORITY DEFAULT_PRIORITY   = HSA_QUEUE_PRIORITY_NORMAL;
    static const unsigned int DEFAULT_QUEUE_PERCENTAGE = 100;
    static const unsigned int ZERO_QUEUE_PERCENTAGE    = 0;

    BaseQueue(void);
    virtual ~BaseQueue(void);

    virtual HSAKMT_STATUS Create(unsigned int NodeId, unsigned int size = DEFAULT_QUEUE_SIZE,
                                 HSAuint64 *pointers = NULL);
    virtual HSAKMT_STATUS Destroy();
    virtual void PlaceAndSubmitPacket(const BasePacket &packet);
    virtual void PlacePacket(const BasePacket &packet);
    virtual void SubmitPacket() = 0;

    int Size() { return m_QueueBuf->Size(); }
    unsigned int GetNodeId() { return m_Node; }
    unsigned int GetFamilyId() { return m_FamilyId; }
    virtual _HSA_QUEUE_TYPE GetQueueType() = 0;

 protected:
    static const unsigned int CMD_NOP_TYPE_2 = 0x80000000;
    static const unsigned int CMD_NOP_TYPE_3 = 0xFFFF1002;

    unsigned int CMD_NOP;
    unsigned int m_pendingWptr;
    HSAuint64 m_pendingWptr64;
    HsaQueueResource m_Resources;
    HsaMemoryBuffer *m_QueueBuf;
    unsigned int m_Node;
    unsigned int m_FamilyId;

    virtual unsigned int Wptr() = 0;
    virtual unsigned int Rptr() = 0;
    virtual unsigned int RptrWhenConsumed() = 0;
    virtual PACKETTYPE PacketTypeSupported() = 0;
};

#endif  // __KFD_BASE_QUEUE__H__
