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

#ifndef __KFD_SDMA_PACKET__H__
#define __KFD_SDMA_PACKET__H__

#include "BasePacket.hpp"
#include "sdma_pkt_struct.h"

// @class SDMAPacket: Marks a group of all SDMA packets
class SDMAPacket : public BasePacket {
 public:
        SDMAPacket(void) {}
        virtual ~SDMAPacket(void) {}

        virtual PACKETTYPE PacketType() const { return PACKETTYPE_SDMA; }
};

class SDMAWriteDataPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMAWriteDataPacket(unsigned int familyId, void* destAddr, unsigned int data);
    SDMAWriteDataPacket(unsigned int familyId, void* destAddr, unsigned int ndw, void *data);

    virtual ~SDMAWriteDataPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }
    // @breif Initialise the packet
    void InitPacket(void* destAddr, unsigned int ndw, void *data);
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_WRITE_UNTILED struct contains all the packet's data
    SDMA_PKT_WRITE_UNTILED *packetData;
    unsigned int packetSize;
};

class SDMACopyDataPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMACopyDataPacket(unsigned int familyId, void *dest, void *src, unsigned int size);
    SDMACopyDataPacket(unsigned int familyId, void *const dst[], void *src, int n, unsigned int surfsize);

    virtual ~SDMACopyDataPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_COPY_LINEAR struct contains all the packet's data
    SDMA_PKT_COPY_LINEAR  *packetData;

    unsigned int packetSize;
};

class SDMAAtomicPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMAAtomicPacket(unsigned int familyId, void *destAddr);
   
    virtual ~SDMAAtomicPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_ATOMIC struct contains all the packet's data
    SDMA_PKT_ATOMIC  *packetData;

    unsigned int packetSize;
};

class SDMAPollPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMAPollPacket(unsigned int familyId, void *pollAddr, uint32_t reference);
   
    virtual ~SDMAPollPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_POLL_REGMEM struct contains all the packet's data
    SDMA_PKT_POLL_REGMEM  *packetData;

    unsigned int packetSize;
};

class SDMASwapDataPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMASwapDataPacket(unsigned int familyId, void *addrA, void *addrB, unsigned int size);

    virtual ~SDMASwapDataPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_COPY_LINEAR struct contains all the packet's data
    SDMA_PKT_SWAP_LINEAR  *packetData;

    unsigned int packetSize;
};

class SDMAFillDataPacket : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMAFillDataPacket(unsigned int familyId, void *dest, unsigned int data, unsigned int size);

    virtual ~SDMAFillDataPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return m_PacketData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return m_PacketSize; }

 protected:
    // SDMA_PKT_CONSTANT_FILL struct contains all the packet's data
    SDMA_PKT_CONSTANT_FILL  *m_PacketData;

    unsigned int m_PacketSize;
};

class SDMAFencePacket : public SDMAPacket {
 public:
    // Empty constructor, before using the packet call the init func
    SDMAFencePacket(void);
    // This contructor will also init the packet, no need for additional calls
    SDMAFencePacket(unsigned int familyId, void* destAddr, unsigned int data);

    virtual ~SDMAFencePacket(void);

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return &packetData; }
    // @brief Initialise the packet
    void InitPacketCI(void* destAddr, unsigned int data);
    void InitPacketNV(void* destAddr, unsigned int data);

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return sizeof(SDMA_PKT_FENCE ); }

 protected:
    // SDMA_PKT_FENCE struct contains all the packet's data
    SDMA_PKT_FENCE  packetData;
};



class SDMAAtomicPacketWrite : public SDMAPacket {
 public:
    // This contructor will also init the packet, no need for additional calls
    SDMAAtomicPacketWrite(unsigned int familyId, void *destAddr, uint64_t data);
   
    virtual ~SDMAAtomicPacketWrite(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return packetData; }

    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 protected:
    // SDMA_PKT_ATOMIC struct contains all the packet's data
    SDMA_PKT_ATOMIC  *packetData;

    unsigned int packetSize;
};


class SDMATrapPacket : public SDMAPacket {
 public:
    // Empty constructor, before using the packet call the init func
    explicit SDMATrapPacket(unsigned int eventID = 0);

    virtual ~SDMATrapPacket(void);

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return &packetData; }
    // @brief Initialise the packet
    void InitPacket(unsigned int eventID);
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return sizeof(SDMA_PKT_TRAP); }

 protected:
    // SDMA_PKT_TRAP struct contains all the packet's data
    SDMA_PKT_TRAP  packetData;
};

class SDMATimePacket : public SDMAPacket {
 public:
    // Empty constructor, before using the packet call the init func
    SDMATimePacket(void*);

    virtual ~SDMATimePacket(void);

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return &packetData; }
    // @brief Initialise the packet
    void InitPacket(void*);
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return sizeof(SDMA_PKT_TIMESTAMP); }

 protected:
    SDMA_PKT_TIMESTAMP  packetData;
};

class SDMASetLocalTimePacket : public SDMAPacket {
 public:
    // Empty constructor, before using the packet call the init func
    SDMASetLocalTimePacket(unsigned long long);

    virtual ~SDMASetLocalTimePacket(void);

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return &packetData; }
    // @brief Initialise the packet
    void InitSetLocalPacket(unsigned long long);
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return sizeof(SDMA_PKT_TIMESTAMP); }

 protected:
    SDMA_PKT_TIMESTAMP  packetData;
};

class SDMALocalTimePacket : public SDMAPacket {
 public:
    // Empty constructor, before using the packet call the init func
    SDMALocalTimePacket(void*);

    virtual ~SDMALocalTimePacket(void);

    // @returns Pointer to the packet
    virtual const void *GetPacket() const  { return &packetData; }
    // @brief Initialise the packet
    void InitLocalPacket(void*);
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return sizeof(SDMA_PKT_TIMESTAMP); }

 protected:
    SDMA_PKT_TIMESTAMP  packetData;
};

class SDMANopPacket : public SDMAPacket {
 public:
    SDMANopPacket(unsigned int count = 1);
    virtual ~SDMANopPacket(void) {}

    // @returns Pointer to the packet
    virtual const void *GetPacket() const { return packetData; }
    // @returns Packet size in bytes
    virtual unsigned int SizeInBytes() const { return packetSize; }

 private:
    SDMA_PKT_NOP *packetData;
    unsigned int packetSize;
};


#endif  // __KFD_SDMA_PACKET__H__
