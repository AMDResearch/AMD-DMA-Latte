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

#ifndef __SDMA_BASE__H__
#define __SDMA_BASE__H__

#include "hsakmt/hsakmt.h"
#include "util.hpp"

class sdmaBase {
 public:
    sdmaBase(void) {}
    ~sdmaBase(void) {}

    unsigned int GetFamilyIdFromNodeId(unsigned int nodeId);
    unsigned int GetFamilyIdFromDefaultNode() { return m_FamilyId; }

 protected:
    HsaVersionInfo  m_VersionInfo;
    HsaSystemProperties m_SystemProperties;
    unsigned int m_FamilyId;
    HsaNodeInfo m_NodeInfo;

    // @brief Executed before every test that uses KFDBaseComponentTest class and sets all common settings for the tests.
    virtual void SetUp();
    // @brief Executed after every test that uses KFDBaseComponentTest class.
    virtual void TearDown();
};

extern sdmaBase* g_baseTest;
#endif  //  __SDMA_BASE__H__