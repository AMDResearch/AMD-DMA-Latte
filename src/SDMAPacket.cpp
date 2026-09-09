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

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "SDMAPacket.hpp"
#include "util.hpp"

/* Byte/dword count in many SDMA packets is 1-based in AI, meaning a
 * count of 1 is encoded as 0.
 */
#define SDMA_COUNT(c) (m_FamilyId < FAMILY_AI ? (c) : (c)-1)

SDMAWriteDataPacket::SDMAWriteDataPacket(unsigned int familyId, void* destAddr, unsigned int data):
    packetData(NULL) {
    m_FamilyId = familyId;
    InitPacket(destAddr, 1, &data);
}

SDMAWriteDataPacket::SDMAWriteDataPacket(unsigned int familyId, void* destAddr, unsigned int ndw,
                                         void *data):
    packetData(NULL) {
    m_FamilyId = familyId;
    InitPacket(destAddr, ndw, data);
}

void SDMAWriteDataPacket::InitPacket(void* destAddr, unsigned int ndw,
                                     void *data) {
    packetSize = sizeof(SDMA_PKT_WRITE_UNTILED) +
        (ndw - 1) * sizeof(unsigned int);
    packetData = reinterpret_cast<SDMA_PKT_WRITE_UNTILED *>(AllocPacket());

    packetData->HEADER_UNION.op = SDMA_OP_WRITE;
    packetData->HEADER_UNION.sub_op = SDMA_SUBOP_WRITE_LINEAR;

    SplitU64(reinterpret_cast<HSAuint64>(destAddr),
             packetData->DST_ADDR_LO_UNION.DW_1_DATA,  // dst_addr_31_0
             packetData->DST_ADDR_HI_UNION.DW_2_DATA);  // dst_addr_63_32

    packetData->DW_3_UNION.count = SDMA_COUNT(ndw);
    memcpy(&packetData->DATA0_UNION.DW_4_DATA, data, ndw*sizeof(unsigned int));
}

#define BITS (21)
#define GBITS (30)
#define TWO_MEG (1 << BITS)
#define ONE_GIG (1 << GBITS)
SDMACopyDataPacket::SDMACopyDataPacket(unsigned int familyId,
                        void *const dsts[], void *src, int n, unsigned int surfsize) {
    int32_t size = 0, i;
    void **dst = reinterpret_cast<void**>(malloc(sizeof(void*) * n));
    const int singlePacketSize = sizeof(SDMA_PKT_COPY_LINEAR) +
                        sizeof(SDMA_PKT_COPY_LINEAR::DST_ADDR[0]) * n;

    if (n > 2)
        WARN() << "SDMACopyDataPacket does not support more than 2 dst addresses!" << std::endl;

    m_FamilyId = familyId;
    memcpy(dst, dsts, sizeof(void*) * n);

    packetSize = ((surfsize + ONE_GIG - 1) >> GBITS) * singlePacketSize;

    SDMA_PKT_COPY_LINEAR *pSDMA = reinterpret_cast<SDMA_PKT_COPY_LINEAR *>(AllocPacket());
    packetData = pSDMA;

	int count=0;

    while (surfsize > 0) {
        /* SDMA support maximum 0x3fffe0 byte in one copy, take 2M here */
        if (surfsize > ONE_GIG)
            size = ONE_GIG;
        else{
            size = surfsize;
	    }

        memset(pSDMA, 0, singlePacketSize);
        pSDMA->HEADER_UNION.op           = SDMA_OP_COPY;
        pSDMA->HEADER_UNION.sub_op       = SDMA_SUBOP_COPY_LINEAR;
        pSDMA->HEADER_UNION.broadcast    = n > 1 ? 1 : 0;
        pSDMA->COUNT_UNION.count_ext.count	= SDMA_COUNT(size);
        SplitU64(reinterpret_cast<HSAuint64>(src),
                 pSDMA->SRC_ADDR_LO_UNION.DW_3_DATA,  // src_addr_31_0
                 pSDMA->SRC_ADDR_HI_UNION.DW_4_DATA);  // src_addr_63_32

        for (i = 0; i < n; i++)
            SplitU64(reinterpret_cast<HSAuint64>(dst[i]),
                    pSDMA->DST_ADDR[i].DST_ADDR_LO_UNION.DW_5_DATA,  // dst_addr_31_0
                    pSDMA->DST_ADDR[i].DST_ADDR_HI_UNION.DW_6_DATA);  // dst_addr_63_32

        pSDMA = reinterpret_cast<SDMA_PKT_COPY_LINEAR *>(reinterpret_cast<char *>(pSDMA) + singlePacketSize);
        for (i = 0; i < n; i++)
            dst[i] = reinterpret_cast<char *>(dst[i]) + size;
        src = reinterpret_cast<char *>(src) + size;
        surfsize -= size;
	count++;
    }
    free(dst);
}

SDMACopyDataPacket::SDMACopyDataPacket(unsigned int familyId, void* dst, void *src, unsigned int surfsize) {
    new (this)SDMACopyDataPacket(familyId, &dst, src, 1, surfsize);
}

SDMAPollPacket::SDMAPollPacket(unsigned int familyId, void *addr, uint32_t reference) {
    SDMA_PKT_POLL_REGMEM *pSDMA;

    m_FamilyId = familyId;
    /* SDMA support maximum 0x3fffe0 byte in one copy. Use 2M copy_size */
    packetSize = sizeof(SDMA_PKT_POLL_REGMEM);
    pSDMA = reinterpret_cast<SDMA_PKT_POLL_REGMEM *>(AllocPacket());
    packetData = pSDMA;

    pSDMA->HEADER_UNION.op = SDMA_OP_POLL_REGMEM;
    pSDMA->HEADER_UNION.mem_poll = 1;
    pSDMA->HEADER_UNION.func = 0x3;  // IsEqual.
    SplitU64(reinterpret_cast<HSAuint64>(addr),
            pSDMA->ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
            pSDMA->ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

    pSDMA->VALUE_UNION.value = reference;

    pSDMA->MASK_UNION.mask = 0xffffffff;  // Compare the whole content.

    pSDMA->DW5_UNION.interval = 0x04;
    pSDMA->DW5_UNION.retry_count = 0xfff;  // Retry forever.
}

SDMASwapDataPacket::SDMASwapDataPacket(unsigned int familyId, void *addrA, void *addrB, unsigned int size) {
    unsigned int swap_size;
    SDMA_PKT_SWAP_LINEAR *pSDMA;

    m_FamilyId = familyId;
    
    //const int singlePacketSize = sizeof(SDMA_PKT_COPY_LINEAR);
    packetSize = ((size + ONE_GIG - 1) >> GBITS) * sizeof(SDMA_PKT_SWAP_LINEAR);
    pSDMA = reinterpret_cast<SDMA_PKT_SWAP_LINEAR *>(AllocPacket());
    packetData = pSDMA;

    while (size > 0) {
        if (size > ONE_GIG)
            swap_size = ONE_GIG;
        else
            swap_size = size;

        //memset(&packetData, 0, singlePacketSize);
        pSDMA->HEADER_UNION.op = SDMA_OP_COPY;
        pSDMA->HEADER_UNION.sub_op = SDMA_SUBOP_SWAP_LINEAR;

        pSDMA->COUNT_UNION.count_ext.count = SDMA_COUNT(swap_size);

        SplitU64(reinterpret_cast<HSAuint64>(addrA),
            pSDMA->ADDR_A_LO_UNION.DW_3_DATA, /*addr_31_0*/
            pSDMA->ADDR_A_HI_UNION.DW_4_DATA); /*addr_63_32*/

        SplitU64(reinterpret_cast<HSAuint64>(addrB),
            pSDMA->ADDR_B_LO_UNION.DW_5_DATA, /*addr_31_0*/
            pSDMA->ADDR_B_HI_UNION.DW_6_DATA); /*addr_63_32*/
            
        pSDMA++;

        addrA = reinterpret_cast<char *>(addrA) + swap_size;
        addrB = reinterpret_cast<char *>(addrB) + swap_size;
        size -= swap_size;
    }
}

SDMAFillDataPacket::SDMAFillDataPacket(unsigned int familyId, void *dst, unsigned int data, unsigned int size) {
    unsigned int copy_size;
    SDMA_PKT_CONSTANT_FILL *pSDMA;

    m_FamilyId = familyId;
    /* SDMA support maximum 0x3fffe0 byte in one copy. Use 2M copy_size */
    m_PacketSize = ((size + ONE_GIG - 1) >> GBITS) * sizeof(SDMA_PKT_CONSTANT_FILL);
    pSDMA = reinterpret_cast<SDMA_PKT_CONSTANT_FILL *>(AllocPacket());
    m_PacketData = pSDMA;

    while (size > 0) {
        if (size > ONE_GIG)
            copy_size = ONE_GIG;
        else
            copy_size = size;

        pSDMA->HEADER_UNION.op = SDMA_OP_CONST_FILL;
        pSDMA->HEADER_UNION.sub_op = 0;

        /* If both size and address are DW aligned, then use DW fill */
        if (!(copy_size & 0x3) && !((HSAuint64)dst & 0x3))
            pSDMA->HEADER_UNION.fillsize = 2; /* DW Fill */
        else
            pSDMA->HEADER_UNION.fillsize = 0; /* Byte Fill */

        pSDMA->COUNT_UNION.count = SDMA_COUNT(copy_size);

        SplitU64(reinterpret_cast<HSAuint64>(dst),
            pSDMA->DST_ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
            pSDMA->DST_ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

        pSDMA->DATA_UNION.DW_3_DATA = data;
        pSDMA++;

        dst = reinterpret_cast<char *>(dst) + copy_size;
        size -= copy_size;
    }
}

SDMAFencePacket::SDMAFencePacket(void) {
}

SDMAFencePacket::SDMAFencePacket(unsigned int familyId, void* destAddr, unsigned int data) {
    m_FamilyId = familyId;
    if (m_FamilyId < FAMILY_NV)
        InitPacketCI(destAddr, data);
    else
        InitPacketNV(destAddr, data);
}

SDMAFencePacket::~SDMAFencePacket(void) {
}

void SDMAFencePacket::InitPacketCI(void* destAddr, unsigned int data) {
    memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_FENCE;

    SplitU64(reinterpret_cast<HSAuint64>(destAddr),
             packetData.ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
             packetData.ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

    packetData.DATA_UNION.data = data;
}

void SDMAFencePacket::InitPacketNV(void * destAddr,unsigned int data) {
    memset(&packetData, 0, SizeInBytes());

    /* GPA=0 becaue we use virtual address
     * Snoop = 1 because we want the write be CPU coherent
     * System = 1 because the memory is system memory
     * mtype = uncached, for the purpose of CPU coherent, L2 policy doesn't matter in this case
     */
    packetData.HEADER_UNION.DW_0_DATA = (0 << 23) | (1 << 22) | (1 << 20) | (3 << 16) | SDMA_OP_FENCE;

    SplitU64(reinterpret_cast<unsigned long long>(destAddr),
             packetData.ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
             packetData.ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

    packetData.DATA_UNION.data = data;
}

SDMAAtomicPacketWrite::SDMAAtomicPacketWrite(unsigned int familyId, void* destAddr, uint64_t data) {
    SDMA_PKT_ATOMIC *pSDMA;
    m_FamilyId = familyId;

    packetSize = sizeof(SDMA_PKT_ATOMIC);
    pSDMA = reinterpret_cast<SDMA_PKT_ATOMIC *>(AllocPacket());
    packetData = pSDMA;
    
    pSDMA->HEADER_UNION.op = SDMA_OP_ATOMIC;
    pSDMA->HEADER_UNION.operation = SDMA_ATOMIC_WRITE;

    SplitU64(reinterpret_cast<HSAuint64>(destAddr),
             pSDMA->ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
             pSDMA->ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

    pSDMA->SRC_DATA_LO_UNION.src_data_31_0 = static_cast<uint32_t>(data);
    pSDMA->SRC_DATA_HI_UNION.src_data_63_32 = static_cast<uint32_t>(data >> 32);
    std::cout << std::hex << "actual data " << data << "Lower bits value " << static_cast<uint32_t>(data) << " upper bits value " << static_cast<uint32_t>(data >> 32) << std::endl;
    //pSDMA->SRC_DATA_LO_UNION.src_data_31_0 = 0x00000001;
    //pSDMA->SRC_DATA_HI_UNION.src_data_63_32 = 0x00000000;
}

/* atomic packet to do plan 64b write
SDMAAtomicPacketWrite::SDMAAtomicPacketWrite(unsigned int familyId, void* destAddr, int data) {
    m_FamilyId = familyId;
    
    //memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_ATOMIC;
    packetData.HEADER_UNION.operation = SDMA_ATOMIC_WRITE64;

    SplitU64(reinterpret_cast<HSAuint64>(destAddr),
             packetData.ADDR_LO_UNION.DW_1_DATA, //dst_addr_31_0
             packetData.ADDR_HI_UNION.DW_2_DATA); //dst_addr_63_32

    packetData.SRC_DATA_LO_UNION.src_data_31_0 = static_cast<uint32_t>(data);
    packetData.SRC_DATA_HI_UNION.src_data_63_32 = static_cast<uint32_t>(data >> 32);
    //packetData.SRC_DATA_LO_UNION.src_data_31_0 = 0x00000001;
    //packetData.SRC_DATA_HI_UNION.src_data_63_32 = 0x00000000;
}

 atomic packet to do plan 64b write
SDMAAtomicPacketWrite::~SDMAAtomicPacketWrite(void) {
}*/

SDMAAtomicPacket::SDMAAtomicPacket(unsigned int familyId, void* destAddr) {
    SDMA_PKT_ATOMIC *pSDMA;
    m_FamilyId = familyId;

    packetSize = sizeof(SDMA_PKT_ATOMIC);
    pSDMA = reinterpret_cast<SDMA_PKT_ATOMIC *>(AllocPacket());
    packetData = pSDMA;
    
    pSDMA->HEADER_UNION.op = SDMA_OP_ATOMIC;
    pSDMA->HEADER_UNION.operation = SDMA_ATOMIC_ADD64;

    SplitU64(reinterpret_cast<HSAuint64>(destAddr),
             pSDMA->ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
             pSDMA->ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/

    pSDMA->SRC_DATA_LO_UNION.src_data_31_0 = 0xffffffff;
    pSDMA->SRC_DATA_HI_UNION.src_data_63_32 = 0xffffffff;
    //pSDMA->SRC_DATA_LO_UNION.src_data_31_0 = 0x00000001;
    //pSDMA->SRC_DATA_HI_UNION.src_data_63_32 = 0x00000000;
}




SDMATrapPacket::SDMATrapPacket(unsigned int eventID) {
    InitPacket(eventID);
}

SDMATrapPacket::~SDMATrapPacket(void) {
}

void SDMATrapPacket::InitPacket(unsigned int eventID) {
    memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_TRAP;
    packetData.INT_CONTEXT_UNION.int_context = eventID;
}

SDMASetLocalTimePacket::SDMASetLocalTimePacket(unsigned long long data) {
      InitSetLocalPacket(data);
}

SDMASetLocalTimePacket::~SDMASetLocalTimePacket(void) {
}

SDMALocalTimePacket::SDMALocalTimePacket(void *destaddr) {
      InitLocalPacket(destaddr);
}

SDMALocalTimePacket::~SDMALocalTimePacket(void) {
}


SDMATimePacket::SDMATimePacket(void *destaddr) {
     InitPacket(destaddr);
}

SDMATimePacket::~SDMATimePacket(void) {
}

void SDMATimePacket::InitPacket(void *destaddr) {
    memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_TIMESTAMP;
    packetData.HEADER_UNION.sub_op = 1 << 1; /* Get Global GPU Timestamp*/

    if (reinterpret_cast<unsigned long long>(destaddr) & 0x1f)
        WARN() << "SDMATimePacket dst address must aligned to 32bytes boundary" << std::endl;

    SplitU64(reinterpret_cast<unsigned long long>(destaddr),
            packetData.ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
            packetData.ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/
}

void SDMALocalTimePacket::InitLocalPacket(void *destaddr) {
    memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_TIMESTAMP;
    packetData.HEADER_UNION.sub_op = 1; /* Get Local GPU Timestamp*/

    if (reinterpret_cast<unsigned long long>(destaddr) & 0x1f)
        WARN() << "SDMATimePacket dst address must aligned to 32bytes boundary" << std::endl;

    SplitU64(reinterpret_cast<unsigned long long>(destaddr),
            packetData.ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
            packetData.ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/
}

void SDMASetLocalTimePacket::InitSetLocalPacket(unsigned long long data) {
    memset(&packetData, 0, SizeInBytes());

    packetData.HEADER_UNION.op = SDMA_OP_TIMESTAMP;
    packetData.HEADER_UNION.sub_op = 0; /* Set Local GPU Timestamp*/

    if (reinterpret_cast<unsigned long long>(data) & 0x1f)
        WARN() << "SDMATimePacket dst address must aligned to 32bytes boundary" << std::endl;

    SplitU64(reinterpret_cast<unsigned long long>(data),
            packetData.ADDR_LO_UNION.DW_1_DATA, /*dst_addr_31_0*/
            packetData.ADDR_HI_UNION.DW_2_DATA); /*dst_addr_63_32*/
}

