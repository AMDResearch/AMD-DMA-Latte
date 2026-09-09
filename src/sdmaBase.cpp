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

#include <syslog.h>

#include "sdmaBase.hpp"
#include "util.hpp"

void sdmaBase::SetUp() {
    CHECK_HSAKMT_SUCCESS(hsaKmtOpenKFD(),"Failed!");
    CHECK_HSAKMT_SUCCESS(hsaKmtGetVersion(&m_VersionInfo),"Failed!");
    memset(&m_SystemProperties, 0, sizeof(m_SystemProperties));

    CHECK_HSAKMT_SUCCESS(hsaKmtAcquireSystemProperties(&m_SystemProperties),"Failed!");
    CHECK_GT(m_SystemProperties.NumNodes, HSAuint32(0), "HSA has no nodes.");

    m_NodeInfo.Init(m_SystemProperties.NumNodes);

    const HsaNodeProperties *nodeProperties = m_NodeInfo.HsaDefaultGPUNodeProperties();
    CHECK_NOTNULL(nodeProperties, "failed to get HSA default GPU Node properties");

    m_FamilyId = FamilyIdFromNode(nodeProperties);

    g_baseTest = this;
}

unsigned int sdmaBase::GetFamilyIdFromNodeId(unsigned int nodeId)
{
    return FamilyIdFromNode(m_NodeInfo.GetNodeProperties(nodeId));
}

void sdmaBase::TearDown() {
    EXPECT_SUCCESS(hsaKmtReleaseSystemProperties(),"Failed!");
    EXPECT_SUCCESS(hsaKmtCloseKFD(),"Failed!");
    g_baseTest = NULL;
}
