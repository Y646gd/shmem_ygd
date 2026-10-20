/** Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * Portable identity test: production body with scalar identity fields.
 */
#include <gtest/gtest.h>
#include "identity_fixture.h"

TEST(TopologyIdentityMatrix, SameServerGroupsAndCrossServerSuperPods)
{
    struct Case { AscendSocType soc; uint32_t remoteSpid, remoteServer, remoteDevice; bool expected; };
    const Case cases[] = {
        {ASCEND_910B, 7, 10, 0, true}, {ASCEND_910B, 7, 10, 7, true},
        {ASCEND_910B, 7, 10, 8, false}, {ASCEND_910B, 7, 10, 15, false},
        {ASCEND_910C, 7, 10, 8, true}, {ASCEND_950, 7, 10, 15, true},
        {ASCEND_910B, 7, 11, 8, true}, {ASCEND_910C, 7, 11, 8, true},
        {ASCEND_950, 7, 11, 15, true}, {ASCEND_910B, 8, 11, 0, false},
        {ASCEND_910C, 8, 11, 0, false}, {ASCEND_950, 8, 11, 0, false},
        {ASCEND_910B, shm::invalidSuperPodId, 11, 0, false},
        {ASCEND_910C, shm::invalidSuperPodId, 11, 0, false},
        {ASCEND_950, shm::invalidSuperPodId, 11, 0, false}
    };
    for (const auto& c : cases) {
        shm::MemSegment segment;
        segment.socType_ = c.soc;
        EXPECT_EQ(segment.IsSdmaAccessible(c.remoteSpid, c.remoteServer, c.remoteDevice), c.expected);
    }
    for (auto soc : {ASCEND_910B, ASCEND_910C, ASCEND_950}) {
        shm::MemSegment segment;
        segment.socType_ = soc;
        segment.superPodId_ = shm::invalidSuperPodId;
        EXPECT_FALSE(segment.IsSdmaAccessible(7, 11, 0));
    }
}
