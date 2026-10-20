/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * Portable matrix: production bodies with segment/transport test doubles.
 */
#include <gtest/gtest.h>
#include "init/shmemi_init.h"
#include "engine_fixture.h"

TEST(TopologyEngineMatrix, ConfigurationReachabilityRouteSelfAndQueryMasks)
{
    // HYBM and transport use the same internal bits, unlike public engine bits.
    static_assert(HYBM_DOP_TYPE_MTE == static_cast<int>(ACLSHMEM_TRANSPORT_MTE));
    static_assert(HYBM_DOP_TYPE_DEVICE_RDMA == static_cast<int>(ACLSHMEM_TRANSPORT_ROCE));
    static_assert(HYBM_DOP_TYPE_DEVICE_SDMA == static_cast<int>(ACLSHMEM_TRANSPORT_SDMA));
    static_assert(HYBM_DOP_TYPE_DEVICE_UDMA == static_cast<int>(ACLSHMEM_TRANSPORT_UDMA));
    aclshmemi_init_backend backend;
    aclshmemx_init_attr_t attr{};
    aclshmem_device_host_state_t state{};
    attr.instance_id = 41;
    attr.n_pes = state.npes = 2;
    ASSERT_EQ(backend.bind_aclshmem_entity(&attr, &state, nullptr, nullptr,
        shm::transport::UdmaQpConfig{}, 1, 1), 0);
    for (uint32_t config = 0; config < 16; ++config) {
#ifndef ACLSHMEM_SOC_950
        // Non-950 UDMA inputs fail extension initialization; no successful query.
        if (config & ACLSHMEM_DATA_OP_UDMA) continue;
#endif
        attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(config);
#ifdef ACLSHMEM_SOC_950
        EXPECT_EQ(CmoQuietMask(&attr), (config & 2U) ? static_cast<uint32_t>(ACLSHMEM_TRANSPORT_SDMA) : 0U);
#else
        EXPECT_EQ(CmoQuietMask(&attr), 0U);
#endif
        for (bool direct : {false, true}) for (bool routed : {false, true})
        for (bool manager : {false, true}) for (uint32_t peer : {0U, 1U}) {
            SCOPED_TRACE(testing::Message() << "config=" << config << " D=" << direct
                << " route=" << routed << " manager=" << manager << " peer=" << peer);
            shm::RouteFixture route{routed};
            shm::MemEntityDefault entity;
            entity.options_ = ConfigureEngines(&attr);
            entity.options_.rankId = 0;
            entity.direct = direct;
            entity.transportManager_ = manager ? &route : nullptr;
            const auto capability = entity.CanReachDataOperators(peer);
            uint32_t expected = (direct ? 1U : 0U) | (config & 4U);
#ifdef ACLSHMEM_SOC_950
            if (direct && peer != 0 && routed && manager) expected |= config & 8U;
#else
            if (direct) expected |= config & 2U;
#endif
            uint32_t internal = (expected & 9U) | ((expected & 2U) << 1U) | ((expected & 4U) >> 1U);
            ASSERT_EQ(static_cast<uint32_t>(capability), internal);
            // A zero remote capability fails real init (-100); never present it
            // as a successful initialized public-query fixture.
            if (peer != 0 && capability == 0) continue;
            if (peer == 0 && !direct) continue; // Normal self memory is reachable.
            state.topo_list[peer] = static_cast<aclshmem_transport_t>(capability);
            for (uint32_t subset = 0; subset < 8; ++subset) {
                std::vector<aclshmemx_net_layer_t> layers;
                const aclshmemx_net_layer_t values[] = {ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_1,
                                                       ACLSHMEM_NET_LAYER_3};
                if (peer != 0) for (uint32_t bit = 0; bit < 3; ++bit)
                    if (subset & (1U << bit)) layers.push_back(values[bit]);
                backend.entity_map_.at(attr.instance_id)->topology_net_layers_by_pe.resize(2);
                backend.entity_map_.at(attr.instance_id)->topology_net_layers_by_pe[peer] = layers;
                for (uint32_t mask : {1U, 2U, 3U}) {
                    aclshmemx_topology_t out = ACLSHMEMX_TOPOLOGY_INITIALIZER;
                    const int status = backend.query_topology(attr.instance_id, peer, mask, &out);
#ifndef ACLSHMEM_SOC_950
                    if (mask & 2U) { EXPECT_EQ(status, ACLSHMEM_NOT_SUPPORTED); continue; }
#endif
                    ASSERT_EQ(status, 0);
                    EXPECT_EQ(out.engine_mask, (mask & 1U) ? expected : 0U);
                    EXPECT_EQ(out.net_layer_count, (mask & 2U) ? layers.size() : 0U);
                    if (!(mask & 2U) || layers.empty()) EXPECT_EQ(out.net_layers, nullptr);
                    else EXPECT_EQ(std::vector<aclshmemx_net_layer_t>(out.net_layers,
                        out.net_layers + out.net_layer_count), layers);
                }
            }
        }
    }
    EXPECT_EQ(backend.release_aclshmem_entity(attr.instance_id), 0);
}

// Explicit RFC 9.2 tables, independent of the broad formula-based oracle above.
TEST(TopologyEngineMatrix, Rfc9LiteralSelfAndRemoteExpectations)
{
#ifdef ACLSHMEM_SOC_950
    const uint32_t self[]   = {1,1,1,5,5,5,5,1,1,1,1,5,5,5,5};
    const uint32_t remote[] = {1,1,1,5,5,5,5,9,9,9,9,13,13,13,13};
#else
    const uint32_t self[]   = {1,3,3,5,5,7,7};
    const uint32_t remote[] = {1,3,3,5,5,7,7};
#endif
    auto to_public = [](uint32_t internal) {
        uint32_t result = 0;
        if (internal & HYBM_DOP_TYPE_MTE) result |= ACLSHMEM_DATA_OP_MTE;
        if (internal & HYBM_DOP_TYPE_DEVICE_SDMA) result |= ACLSHMEM_DATA_OP_SDMA;
        if (internal & HYBM_DOP_TYPE_DEVICE_RDMA) result |= ACLSHMEM_DATA_OP_ROCE;
        if (internal & HYBM_DOP_TYPE_DEVICE_UDMA) result |= ACLSHMEM_DATA_OP_UDMA;
        return result;
    };
    for (uint32_t config = 1; config <= sizeof(self)/sizeof(self[0]); ++config) {
        SCOPED_TRACE(config);
        aclshmemx_init_attr_t attr{};
        attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(config);
        shm::MemEntityDefault entity; entity.options_ = ConfigureEngines(&attr);
        shm::RouteFixture route{true}; entity.transportManager_ = &route;
        entity.direct = true; entity.options_.rankId = 0;
        EXPECT_EQ(to_public(entity.CanReachDataOperators(0)), self[config-1]);
        EXPECT_EQ(to_public(entity.CanReachDataOperators(1)), remote[config-1]);
        entity.direct = false;
        EXPECT_EQ(to_public(entity.CanReachDataOperators(1)), (config & 4U) ? 4U : 0U);
    }
}
TEST(TopologyEngineMatrix, Rfc9ExtensionFailuresAreNotSuccessfulInitializedQueries)
{
    shm::MemEntityDefault entity;
    aclshmemx_init_attr_t attr{};
    attr.option_attr.data_op_engine_type = ACLSHMEM_DATA_OP_UDMA;
    entity.options_ = ConfigureEngines(&attr); shm::DlApi::status = 0;
#ifdef ACLSHMEM_SOC_950
    EXPECT_EQ(entity.LoadExtendLibrary(), 0);
    shm::DlApi::status = -9;
    EXPECT_EQ(entity.LoadExtendLibrary(), -9);
#else
    EXPECT_EQ(entity.LoadExtendLibrary(), ACLSHMEM_NOT_SUPPORTED);
#endif
    shm::DlApi::status = -9;
    attr.option_attr.data_op_engine_type = ACLSHMEM_DATA_OP_ROCE;
    entity.options_ = ConfigureEngines(&attr);
    EXPECT_EQ(entity.LoadExtendLibrary(), -9);
    shm::DlApi::status = 0;
}
