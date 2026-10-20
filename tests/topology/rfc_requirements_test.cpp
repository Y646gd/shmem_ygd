/** Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * RFC contract tests against production query/build bodies and minimal instance storage.
 * Bind/release below are fixture lifecycle, not full device init/finalize.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include "init/shmemi_init.h"

namespace {
using namespace shm::transport;
class RfcContractTest : public testing::Test {
protected:
    aclshmemi_init_backend backend;
    aclshmem_device_host_state_t state{};
    aclshmemx_init_attr_t attr{};
    aclshmem_instance_ctx instance{81, nullptr};
    aclshmemi_bootstrap_handle_t boot{};
    decltype(g_state) saved = g_state;
    aclshmemi_init_backend* saved_manager = init_manager;
    aclshmem_instance_ctx* saved_instance = g_instance_ctx;
    int gathers = 0;
    void SetUp() override {
        attr.instance_id = instance.id; attr.n_pes = state.npes = 3;
        attr.my_pe = state.mype = 1;
        boot.npes = 3; boot.mype = 1; boot.bootstrap_state = &gathers;
        boot.allgather = [](const void* in, void* out, int bytes, aclshmemi_bootstrap_handle_t* b) {
            ++*static_cast<int*>(b->bootstrap_state);
            if (bytes != sizeof(int)) return -1;
            for (int p = 0; p < b->npes; ++p) static_cast<int*>(out)[p] = *static_cast<const int*>(in);
            return 0;
        };
        ASSERT_EQ(backend.bind_aclshmem_entity(&attr, &state, &boot, nullptr, UdmaQpConfig{}, 1, 1), 0);
        init_manager = &backend; g_instance_ctx = &instance;
        g_state.npes = 3; g_state.mype = 1; g_state.is_aclshmem_initialized = true;
        state.topo_list[0] = ACLSHMEM_TRANSPORT_ROCE;
        state.topo_list[1] = ACLSHMEM_TRANSPORT_MTE;
        state.topo_list[2] = ACLSHMEM_TRANSPORT_UDMA;
    }
    void TearDown() override {
        for (auto& item : backend.entity_map_) delete item.second;
        backend.entity_map_.clear(); g_state = saved;
        init_manager = saved_manager; g_instance_ctx = saved_instance;
    }
};

TEST_F(RfcContractTest, Rfc1AbiInitializerAndEveryFieldOffset) {
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    EXPECT_EQ(sizeof(v), 64U); EXPECT_EQ(alignof(decltype(v)), 8U);
    EXPECT_EQ(offsetof(decltype(v), struct_size), 0U);
    EXPECT_EQ(offsetof(decltype(v), abi_version), 4U);
    EXPECT_EQ(offsetof(decltype(v), engine_mask), 8U);
    EXPECT_EQ(offsetof(decltype(v), net_layer_count), 12U);
    EXPECT_EQ(offsetof(decltype(v), net_layers), 16U);
    EXPECT_EQ(offsetof(decltype(v), reserved), 24U);
    EXPECT_EQ(v.struct_size, 64U); EXPECT_EQ(v.abi_version, 1U);
    EXPECT_EQ(v.engine_mask, 0U); EXPECT_EQ(v.net_layer_count, 0U); EXPECT_EQ(v.net_layers, nullptr);
    for (auto r : v.reserved) EXPECT_EQ(r, 0U);
}

TEST_F(RfcContractTest, Rfc1AllInvalidBitsSizesVersionsAndMissingActiveState) {
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    for (uint32_t bit = 2; bit < 32; ++bit) {
        EXPECT_EQ(aclshmemx_query_topology(0, 1U << bit, &v), ACLSHMEM_INVALID_VALUE);
        EXPECT_EQ(aclshmemx_query_topology(0, (1U << bit) | 3U, &v), ACLSHMEM_INVALID_VALUE);
    }
    for (uint32_t size = 0; size < 64; ++size) {
        v.struct_size = size;
        EXPECT_EQ(aclshmemx_query_topology(0, 1, &v), ACLSHMEM_INVALID_VALUE);
    }
    v.struct_size = 64;
    for (uint32_t version : {0U, 2U, UINT32_MAX}) {
        v.abi_version = version;
        EXPECT_EQ(aclshmemx_query_topology(0, 1, &v), ACLSHMEM_INVALID_VALUE);
    }
    v.abi_version = 1;
    init_manager = nullptr;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &v), ACLSHMEM_NOT_INITED);
    init_manager = &backend; g_instance_ctx = nullptr;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &v), ACLSHMEM_NOT_INITED);
    g_instance_ctx = &instance; instance.id = 999;
    EXPECT_EQ(aclshmemx_query_topology(0, 1, &v), ACLSHMEM_NOT_INITED);
    instance.id = 81;
}

TEST_F(RfcContractTest, Rfc1CurrentInstanceWorldPeAndQueryPerformsNoCollectives) {
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    const uint32_t expected[] = {4, 1, 8};
    for (int repeat = 0; repeat < 20; ++repeat) for (int peer = 0; peer < 3; ++peer) {
        ASSERT_EQ(aclshmemx_query_topology(peer, 1, &v), 0);
        EXPECT_EQ(v.engine_mask, expected[peer]);
    }
    EXPECT_EQ(gathers, 0);
}

TEST_F(RfcContractTest, Rfc10GateChecksEveryRemoteAndExcludesActualSelf) {
    state.topo_list[1] = static_cast<aclshmem_transport_t>(0);
    EXPECT_EQ(backend.validate_peer_reachability(81), 0);
    for (int target : {0, 2}) {
        const auto old = state.topo_list[target]; state.topo_list[target] = static_cast<aclshmem_transport_t>(0);
        EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_NOT_SUPPORTED);
        state.topo_list[target] = old;
    }
    EXPECT_EQ(gathers, 3);
    for (int count : {0, -1, ACLSHMEM_MAX_PES + 1}) {
        state.npes = count;
        EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_INVALID_PARAM);
    }
    state.npes = 3; state.mype = 3;
    EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_INVALID_PARAM);
    EXPECT_EQ(gathers, 3);
}

#ifdef ACLSHMEM_SOC_950
std::unique_ptr<TopologyContext> RoceContext(uint32_t source, const std::string& domain = "domain") {
    auto c = std::make_unique<TopologyContext>(); c->my_pe = source;
    c->rank_to_local_id = {7, 3, 9}; c->rank_to_sync_endpoints.resize(3);
    for (uint32_t p = 0; p < 3; ++p) {
        SyncEndpoint ep{3, domain, NetType::Clos, TopoFabric::Clos, "plane", {"d2h"}, 0};
        ep.addrType = RankAddrType::IPV4; c->rank_to_sync_endpoints[p].push_back(ep);
    }
    return c;
}

TEST_F(RfcContractTest, Rfc1AllMasksPreserveAllReservedAndExtendedTail) {
    auto& e = *backend.entity_map_.at(81);
    e.topology_context = RoceContext(1);
    ASSERT_EQ(backend.build_topology_state(81), 0);
    struct Extended { aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER; std::array<uint64_t, 4> tail; } out;
    for (uint32_t mask : {1U, 2U, 3U}) for (int peer : {0, 1}) {
        out.v.struct_size = sizeof(out); out.v.abi_version = 1;
        for (uint32_t i = 0; i < 5; ++i) out.v.reserved[i] = 100 + i;
        out.tail.fill(0xcafef00dULL);
        // Poison previous outputs so unrequested fields must actively be cleared.
        out.v.engine_mask = 0xffffffffU; out.v.net_layer_count = 77;
        ASSERT_EQ(aclshmemx_query_topology(peer, mask, &out.v), 0);
        EXPECT_EQ(out.v.struct_size, sizeof(out)); EXPECT_EQ(out.v.abi_version, 1U);
        for (uint32_t i = 0; i < 5; ++i) EXPECT_EQ(out.v.reserved[i], 100U + i);
        for (auto t : out.tail) EXPECT_EQ(t, 0xcafef00dULL);
        EXPECT_EQ(out.v.engine_mask, (mask & 1U) ? (peer == 0 ? 4U : 1U) : 0U);
        const bool has_layer = (mask & 2U) && peer != 1;
        EXPECT_EQ(out.v.net_layer_count, has_layer ? 1U : 0U);
        if (has_layer) { ASSERT_NE(out.v.net_layers, nullptr); EXPECT_EQ(out.v.net_layers[0], ACLSHMEM_NET_LAYER_3); }
        else EXPECT_EQ(out.v.net_layers, nullptr);
    }
}

TEST_F(RfcContractTest, Rfc4SnapshotIsStableUntilFixtureOwnerRelease) {
    auto& e = *backend.entity_map_.at(81); e.topology_context = RoceContext(1);
    ASSERT_EQ(backend.build_topology_state(81), 0);
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    ASSERT_EQ(aclshmemx_query_topology(0, 2, &v), 0);
    const auto* borrowed = v.net_layers;
    std::vector<aclshmemx_net_layer_t> copy(borrowed, borrowed + v.net_layer_count);
    e.topology_context->rank_to_sync_endpoints.clear(); // Query must use the published snapshot.
    const int calls = gathers;
    for (int n = 0; n < 20; ++n) {
        ASSERT_EQ(aclshmemx_query_topology(0, 2, &v), 0);
        EXPECT_EQ(v.net_layers, borrowed); EXPECT_EQ(v.net_layers[0], ACLSHMEM_NET_LAYER_3);
    }
    EXPECT_EQ(gathers, calls);
    ASSERT_EQ(backend.release_aclshmem_entity(81), 0);
    EXPECT_EQ(aclshmemx_query_topology(0, 2, &v), ACLSHMEM_NOT_INITED);
    EXPECT_EQ(copy, (std::vector<aclshmemx_net_layer_t>{ACLSHMEM_NET_LAYER_3}));
    // Do not dereference borrowed after release.
}

TEST_F(RfcContractTest, Rfc4ActiveInstanceSwitchAndRepeatedFixtureCreation) {
    for (int cycle = 0; cycle < 4; ++cycle) {
        aclshmem_device_host_state_t other{}; other.npes = 3; other.mype = 0;
        other.topo_list[0] = ACLSHMEM_TRANSPORT_ROCE;
        aclshmemx_init_attr_t other_attr{}; other_attr.instance_id = 82; other_attr.n_pes = 3;
        ASSERT_EQ(backend.bind_aclshmem_entity(&other_attr, &other, &boot, nullptr, UdmaQpConfig{}, 1, 1), 0);
        auto& a = *backend.entity_map_.at(81); auto& b = *backend.entity_map_.at(82);
        a.topology_context = RoceContext(1); b.topology_context = RoceContext(0);
        b.topology_context->rank_to_sync_endpoints[1][0].netInstanceId = "isolated";
        ASSERT_EQ(backend.build_topology_state(81), 0); ASSERT_EQ(backend.build_topology_state(82), 0);
        aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
        instance.id = 81; ASSERT_EQ(aclshmemx_query_topology(0, 3, &v), 0); ASSERT_EQ(v.net_layer_count, 1U);
        const auto* first = v.net_layers;
        instance.id = 82; ASSERT_EQ(aclshmemx_query_topology(0, 3, &v), 0); EXPECT_EQ(v.net_layer_count, 0U);
        instance.id = 81; ASSERT_EQ(aclshmemx_query_topology(0, 3, &v), 0); EXPECT_EQ(v.net_layers, first);
        ASSERT_EQ(backend.release_aclshmem_entity(82), 0);
    }
}

TEST_F(RfcContractTest, Rfc4MissingContextStillAgreesBeforeReturning) {
    EXPECT_EQ(backend.build_topology_state(81), ACLSHMEM_NOT_INITED);
    EXPECT_EQ(gathers, 1);
    EXPECT_EQ(backend.build_topology_state(99), ACLSHMEM_NOT_INITED);
    EXPECT_EQ(gathers, 1);
}
#endif
} // namespace
