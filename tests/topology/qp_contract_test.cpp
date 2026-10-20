/** Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * Portable public setter/getter bodies; resource lookup is a backend test double.
 */
#include <gtest/gtest.h>
#include "init/shmemi_init.h"
extern void ConfigureQpPolicyForTest(bool, uint32_t, bool);

TEST(TopologyQpContract, PhysicalLayerThreeDoesNotEnableQpLayerThree)
{
    ConfigureQpPolicyForTest(true, 4, false);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_0), 0);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 1, ACLSHMEM_NET_LAYER_1), 0);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_3), ACLSHMEM_NOT_SUPPORTED);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_2), ACLSHMEM_NOT_SUPPORTED);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 4, ACLSHMEM_NET_LAYER_0), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_ROCE, 0, ACLSHMEM_NET_LAYER_0), ACLSHMEM_NOT_SUPPORTED);
    ConfigureQpPolicyForTest(true, 4, true);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_0), ACLSHMEM_NOT_SUPPORTED);
    ConfigureQpPolicyForTest(false, 4, false);
    EXPECT_EQ(aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_0), ACLSHMEM_NOT_SUPPORTED);
}

TEST(TopologyQpContract, PublicGetterReturnsBackendLayerAndPreservesOutputOnErrors)
{
    auto* savedManager = init_manager;
    auto* savedInstance = g_instance_ctx;
    const auto savedNpes = g_state.npes;
    aclshmemi_init_backend backend;
    aclshmem_instance_ctx instance{71, nullptr};
    backend.test_qp_layers = {0, 1, 0, 1};
    init_manager = &backend;
    g_instance_ctx = &instance;
    g_state.npes = 2;
    for (uint32_t qp = 0; qp < 4; ++qp) {
        aclshmemx_net_layer_t layer = ACLSHMEM_NET_LAYER_3;
        EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 1, qp, &layer), 0);
        EXPECT_EQ(static_cast<uint32_t>(layer), backend.test_qp_layers[qp]);
    }
    aclshmemx_net_layer_t layer = ACLSHMEM_NET_LAYER_3;
    EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 1, 4, &layer), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(layer, ACLSHMEM_NET_LAYER_3);
    EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, -1, 0, &layer), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 1, 0, nullptr), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_ROCE, 1, 0, &layer), ACLSHMEM_NOT_SUPPORTED);
    init_manager = nullptr;
    EXPECT_EQ(aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 1, 0, &layer), ACLSHMEM_NOT_INITED);
    init_manager = savedManager;
    g_instance_ctx = savedInstance;
    g_state.npes = savedNpes;
}
