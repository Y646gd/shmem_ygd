/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#include <gtest/gtest.h>
#include <cstddef>
#include "host/init/shmem_host_init.h"
#include "init/shmemi_init.h"
#include "transport_def.h"

namespace {
class TopologyQueryTest : public testing::Test {
protected:
    aclshmemi_init_backend backend;
    aclshmemx_init_attr_t attr{};
    aclshmem_device_host_state_t state{};
    aclshmem_instance_ctx instance{23, nullptr};
    aclshmemi_bootstrap_handle_t boot{};
    int remote_status = 0;
    aclshmemi_init_backend* saved_manager{};
    aclshmem_instance_ctx* saved_instance{};
    int saved_npes{};
    bool saved_initialized{};
    void SetUp() override
    {
        saved_manager = init_manager;
        saved_instance = g_instance_ctx;
        saved_npes = g_state.npes;
        saved_initialized = g_state.is_aclshmem_initialized;
        attr.instance_id = instance.id;
        attr.n_pes = 2;
        state.npes = 2;
        state.mype = 0;
        boot.npes = 2;
        boot.bootstrap_state = &remote_status;
        boot.allgather = [](const void* send, void* receive, int, aclshmemi_bootstrap_handle_t* handle) {
            auto* statuses = static_cast<int*>(receive);
            statuses[0] = *static_cast<const int*>(send);
            statuses[1] = *static_cast<int*>(handle->bootstrap_state);
            return 0;
        };
        ASSERT_EQ(backend.bind_aclshmem_entity(&attr, &state, &boot, nullptr,
            shm::transport::UdmaQpConfig{}, 1, 1), ACLSHMEM_SUCCESS);
        init_manager = &backend;
        g_instance_ctx = &instance;
        g_state.npes = 2;
        g_state.is_aclshmem_initialized = true;
    }
    void TearDown() override
    {
        backend.release_aclshmem_entity(instance.id);
        init_manager = saved_manager;
        g_instance_ctx = saved_instance;
        g_state.npes = saved_npes;
        g_state.is_aclshmem_initialized = saved_initialized;
    }
};

TEST_F(TopologyQueryTest, RejectsZeroRemoteCapabilityAndAgreesRemoteFailures)
{
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_NOT_SUPPORTED);
    // The local rank is not a remote transport requirement.
    state.topo_list[1] = ACLSHMEM_TRANSPORT_ROCE;
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_SUCCESS);
    remote_status = ACLSHMEM_NOT_SUPPORTED;
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_NOT_SUPPORTED);
    remote_status = ACLSHMEM_MALLOC_FAILED;
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_MALLOC_FAILED);
    boot.allgather = [](const void*, void*, int, aclshmemi_bootstrap_handle_t*) { return -1; };
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_BOOTSTRAP_ERROR);
}

TEST_F(TopologyQueryTest, ReachabilityGateChecksInstanceAndBootstrap)
{
    EXPECT_EQ(backend.validate_peer_reachability(instance.id + 1), ACLSHMEM_NOT_INITED);
    boot.allgather = nullptr;
    EXPECT_EQ(backend.validate_peer_reachability(instance.id), ACLSHMEM_INVALID_PARAM);
}

TEST_F(TopologyQueryTest, ConvertsEveryInternalBitAndPreservesCallerLayout)
{
    struct Extended {
        aclshmemx_topology_t value = ACLSHMEMX_TOPOLOGY_INITIALIZER;
        uint64_t tail = 0x123456789ULL;
    } out;
    out.value.struct_size = sizeof(out);
    out.value.reserved[3] = 0xabcdef;
    EXPECT_EQ(sizeof(aclshmemx_topology_t), 64U);
    EXPECT_EQ(offsetof(aclshmemx_topology_t, reserved), 24U);
    const uint32_t expected[] = {ACLSHMEM_DATA_OP_MTE, ACLSHMEM_DATA_OP_ROCE,
                                ACLSHMEM_DATA_OP_SDMA, ACLSHMEM_DATA_OP_UDMA};
    for (uint32_t bits = 0; bits < 16; ++bits) {
        state.topo_list[1] = static_cast<aclshmem_transport_t>(bits);
        uint32_t mask = 0;
        for (uint32_t bit = 0; bit < 4; ++bit) if (bits & (1U << bit)) mask |= expected[bit];
        ASSERT_EQ(aclshmemx_query_topology(1, ACLSHMEMX_TOPOLOGY_QUERY_ENGINES, &out.value), ACLSHMEM_SUCCESS);
        EXPECT_EQ(out.value.engine_mask, mask);
        EXPECT_EQ(out.value.net_layer_count, 0U);
        EXPECT_EQ(out.value.net_layers, nullptr);
        EXPECT_EQ(out.value.struct_size, sizeof(out));
        EXPECT_EQ(out.value.abi_version, ACLSHMEMX_TOPOLOGY_ABI_VERSION);
        EXPECT_EQ(out.value.reserved[3], 0xabcdefU);
        EXPECT_EQ(out.tail, 0x123456789ULL);
    }
}

TEST_F(TopologyQueryTest, ChecksParametersAndInitialization)
{
    aclshmemx_topology_t out = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, nullptr), ACLSHMEM_INVALID_PARAM);
    EXPECT_EQ(aclshmemx_query_topology(0, 0, &out), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_query_topology(0, 4, &out), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_query_topology(0, 5, &out), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_query_topology(-1, 1, &out), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(aclshmemx_query_topology(2, 1, &out), ACLSHMEM_INVALID_VALUE);
    out.struct_size = 63;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &out), ACLSHMEM_INVALID_VALUE);
    out.struct_size = 64;
    out.abi_version = 2;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &out), ACLSHMEM_INVALID_VALUE);
    out.abi_version = 1;
    g_state.is_aclshmem_initialized = false;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &out), ACLSHMEM_NOT_INITED);
}

#ifndef ACLSHMEM_SOC_950
TEST_F(TopologyQueryTest, A2A3RejectsLinksEvenWhenEnginesIsAlsoRequested)
{
    aclshmemx_topology_t out = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    EXPECT_EQ(aclshmemx_query_topology(1, 2, &out), ACLSHMEM_NOT_SUPPORTED);
    EXPECT_EQ(aclshmemx_query_topology(1, 3, &out), ACLSHMEM_NOT_SUPPORTED);
}
#endif
} // namespace
