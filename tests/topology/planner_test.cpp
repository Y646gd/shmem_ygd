/** Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * Real sample planner / slice functions; topology and resource APIs are mocks.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include "planner_fixture.h"
#include "examples/topology_query/topology_plan.h"
namespace {
struct ResourceMock {
    int query_status = 0, count_status = 0, layer_status = 0;
    uint32_t engines = ACLSHMEM_DATA_OP_UDMA, count = 2;
    std::vector<aclshmemx_net_layer_t> physical{ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_1};
    std::vector<aclshmemx_net_layer_t> routes{ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_1};
    int queries = 0, counts = 0, gets = 0, target = -1;
} resource;
class RfcPlannerTest : public testing::Test {
    void SetUp() override { resource = ResourceMock{}; }
};
}
extern "C" int aclshmemx_query_topology(int pe, uint32_t mask, aclshmemx_topology_t* out) {
    ++resource.queries; resource.target = pe;
    EXPECT_EQ(mask, 3U);
    if (resource.query_status) return resource.query_status;
    out->engine_mask = resource.engines;
    out->net_layer_count = static_cast<uint32_t>(resource.physical.size());
    out->net_layers = resource.physical.empty() ? nullptr : resource.physical.data();
    return 0;
}
extern "C" int aclshmemx_get_qp_num(data_op_engine_type_t engine, uint32_t* count) {
    EXPECT_EQ(engine, ACLSHMEM_DATA_OP_UDMA); ++resource.counts;
    *count = resource.count; return resource.count_status;
}
extern "C" int aclshmemx_get_qp_net_layer(data_op_engine_type_t engine, int peer,
                                          uint32_t qp, aclshmemx_net_layer_t* layer) {
    EXPECT_EQ(engine, ACLSHMEM_DATA_OP_UDMA); EXPECT_EQ(peer, resource.target); ++resource.gets;
    if (resource.layer_status) return resource.layer_status;
    if (qp >= resource.routes.size()) return ACLSHMEM_INVALID_VALUE;
    *layer = resource.routes[qp]; return 0;
}
TEST_F(RfcPlannerTest, Rfc2RequiresUdmaAndBothPhysicalLayersBeforeResourceLookup) {
    resource.engines = ACLSHMEM_DATA_OP_MTE;
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED);
    EXPECT_EQ(resource.counts, 0); EXPECT_EQ(resource.gets, 0);
    resource.engines = ACLSHMEM_DATA_OP_UDMA;
    for (const auto& layers : std::vector<std::vector<aclshmemx_net_layer_t>>{
            {}, {ACLSHMEM_NET_LAYER_0}, {ACLSHMEM_NET_LAYER_1}, {ACLSHMEM_NET_LAYER_3}}) {
        resource.physical = layers;
        EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED);
    }
    EXPECT_EQ(resource.counts, 0); EXPECT_EQ(resource.gets, 0);
}
TEST_F(RfcPlannerTest, Rfc2TwoQpsOnSameLayerDoNotProveTwoLayerExecution) {
    resource.routes = {ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_0};
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED);
    resource.routes = {ACLSHMEM_NET_LAYER_1, ACLSHMEM_NET_LAYER_0};
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED); // Current sample fixes the QP mapping.
    resource.routes = {ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_1};
    EXPECT_EQ(VerifyTwoLayerPlan(7), 0);
}
TEST_F(RfcPlannerTest, Rfc2SampleRejectsQpCountsOtherThanTwo) {
    for (uint32_t count : {0U, 1U, 3U, 4U}) {
        resource.count = count;
        EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_INVALID_VALUE);
    }
    EXPECT_EQ(resource.gets, 0);
}
TEST_F(RfcPlannerTest, Rfc2PropagatesQueryAndResourceErrors) {
    resource.query_status = ACLSHMEM_NOT_INITED;
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_INITED);
    EXPECT_EQ(resource.counts, 0);
    resource.query_status = 0; resource.count_status = ACLSHMEM_INNER_ERROR;
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(resource.gets, 0);
    resource.count_status = 0; resource.layer_status = ACLSHMEM_BOOTSTRAP_ERROR;
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_BOOTSTRAP_ERROR);
}
TEST_F(RfcPlannerTest, Rfc2ExtraRoceLayerDoesNotAffectValidTwoLayerResources) {
    resource.physical.push_back(ACLSHMEM_NET_LAYER_3);
    EXPECT_EQ(VerifyTwoLayerPlan(12), 0);
    EXPECT_EQ(resource.target, 12); EXPECT_EQ(resource.queries, 1);
    EXPECT_EQ(resource.counts, 1); EXPECT_EQ(resource.gets, 2);
}
TEST(RfcSlicesTest, Rfc2HostAndKernelCoverAllElementsWithoutOverlap) {
    std::vector<uint64_t> lengths{0, 1, 2, 3, 8, 9, 1023, 1024, 1025, UINT64_MAX};
    for (uint64_t total : lengths) for (uint32_t qps : {1U, 2U, 4U}) {
        SCOPED_TRACE(testing::Message() << "N=" << total << " qps=" << qps);
        const auto slices = BuildQpSlices(total, qps);
        uint64_t cursor = 0, minimum = UINT64_MAX, maximum = 0;
        ASSERT_EQ(slices.size(), qps);
        for (uint32_t qp = 0; qp < qps; ++qp) {
            const auto& slice = slices[qp];
            EXPECT_EQ(slice.offset, cursor); // Every interval starts where the previous one ends.
            ASSERT_LE(slice.length, total - cursor); cursor += slice.length;
            minimum = std::min(minimum, slice.length); maximum = std::max(maximum, slice.length);
            const auto device = KernelSliceForTest(total, qp, qps);
            EXPECT_EQ(device.offset, slice.offset); EXPECT_EQ(device.length, slice.length);
        }
        EXPECT_EQ(cursor, total); EXPECT_LE(maximum - minimum, 1U);
    }
    const auto odd = BuildQpSlices(9, 2);
    EXPECT_EQ(odd[0].length, 5U); EXPECT_EQ(odd[1].offset, 5U); EXPECT_EQ(odd[1].length, 4U);
}
