/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * Portable fixture: uses real discovery/parser, mocked driver/bootstrap.
 */
#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include "init/shmemi_init.h"

std::string generated_root;
extern "C" int topo_addr_info_get_size(int, size_t* size) { *size = generated_root.size() + 1; return 0; }
extern "C" int topo_addr_info_get(int, char* data, size_t* size)
{
    if (*size < generated_root.size() + 1) return -1;
    std::memcpy(data, generated_root.c_str(), generated_root.size() + 1);
    return 0;
}

namespace {
using namespace shm::transport;
struct Exchange {
    int calls{0}, fail_at{0}, remote_error_at{0}, corrupt_at{0};
    static int Gather(const void* send, void* recv, int bytes, aclshmemi_bootstrap_handle_t* boot)
    {
        auto& exchange = *static_cast<Exchange*>(boot->bootstrap_state);
        ++exchange.calls;
        if (exchange.calls == exchange.fail_at) return -1;
        for (int pe = 0; pe < boot->npes; ++pe) {
            std::memcpy(static_cast<char*>(recv) + pe * bytes, send, bytes);
        }
        if (exchange.calls == exchange.remote_error_at) static_cast<int*>(recv)[1] = ACLSHMEM_MALLOC_FAILED;
        if (exchange.calls == exchange.corrupt_at) static_cast<unsigned char*>(recv)[0] = 0xc1;
        return 0;
    }
};
class DiscoveryTest : public testing::Test {
protected:
    Exchange exchange;
    aclshmemi_bootstrap_handle_t boot{};
    void SetUp() override
    {
        // Do not overwrite a real deployment's rootinfo.
        if (std::ifstream(TopoReader::ROOTINFO_PATH).good()) GTEST_SKIP() << "requires generated-rootinfo fixture";
        const std::string path = std::string(std::getenv("SHMEM_TOPOLOGY_TEST_DIR")) + "/topo.json";
        std::ofstream(path) << R"({"edge_list":[{"local_a":0,"local_b":1,"local_a_ports":["a"],"local_b_ports":["b"]}]})";
        const nlohmann::json address = {{"addr_type", "IPV4"}, {"addr", "192.0.2.1"}, {"plane_id", "left"}, {"ports", {"d2h"}}};
        const nlohmann::json level = {{"net_layer", 3}, {"net_type", "CLOS"}, {"net_instance_id", "cluster"},
                                     {"rank_addr_list", nlohmann::json::array({address})}};
        const nlohmann::json rank = {{"device_id", 0}, {"local_id", 0}, {"level_list", nlohmann::json::array({level})}};
        generated_root = nlohmann::json{{"topo_file_path", path}, {"rank_list", nlohmann::json::array({rank})}}.dump();
        boot.npes = 2;
        boot.bootstrap_state = &exchange;
        boot.allgather = Exchange::Gather;
    }
};
TEST_F(DiscoveryTest, DiscoversIpOnlyTopologyWithoutCreatingUdmaResources)
{
    TopologyContext context;
    ASSERT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_SUCCESS);
    EXPECT_EQ(exchange.calls, 7); // Four payload exchanges + three failure agreements.
    EXPECT_EQ(context.max_eid_slot_count, 1U);
    ASSERT_EQ(context.rank_to_sync_endpoints.size(), 2U);
    TopoQuerier query(context.root_info, context.topo_info, 0, context.rank_to_local_id, context.rank_to_sync_endpoints);
    std::vector<uint32_t> layers;
    query.GetReachableNetLayers(1, layers);
    EXPECT_EQ(layers, (std::vector<uint32_t>{3}));
}
TEST_F(DiscoveryTest, EveryCollectiveFailureStopsFurtherExchanges)
{
    for (int phase = 1; phase <= 7; ++phase) {
        exchange = {};
        exchange.fail_at = phase;
        TopologyContext context;
        EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_BOOTSTRAP_ERROR) << phase;
        EXPECT_EQ(exchange.calls, phase);
    }
}
TEST_F(DiscoveryTest, RemoteAllocationFailurePreventsNextPayloadExchange)
{
    for (int phase : {1, 5}) {
        exchange = {};
        exchange.remote_error_at = phase;
        TopologyContext context;
        EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_MALLOC_FAILED);
        EXPECT_EQ(exchange.calls, phase);
    }
}
TEST_F(DiscoveryTest, InvalidDescriptionAndDecodeFailureAreAgreed)
{
    TopologyContext context;
    exchange.corrupt_at = 6;
    EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(exchange.calls, 7);
    exchange = {};
    generated_root = "{}";
    EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(exchange.calls, 1);
}
#ifdef ACLSHMEM_SOC_950
TEST_F(DiscoveryTest, CacheIsInstanceOwnedAndQueriesNeverExchange)
{
    aclshmemi_init_backend backend;
    aclshmem_device_host_state_t state{};
    state.npes = 2;
    state.mype = 0;
    entity_member first, second;
    for (auto* item : {&first, &second}) {
        item->entity_host_state = &state;
        item->entity_boot_handle = &boot;
        item->topology_context = std::make_unique<TopologyContext>();
        ASSERT_EQ(item->topology_context->Initialize(0, 0, 2, &boot), ACLSHMEM_SUCCESS);
    }
    second.topology_context->rank_to_sync_endpoints[1][0].netInstanceId = "isolated";
    backend.entity_map_ = {{1, &first}, {2, &second}};
    ASSERT_EQ(backend.build_topology_state(1), 0);
    ASSERT_EQ(backend.build_topology_state(2), 0);
    const auto calls = exchange.calls;
    aclshmemx_topology_t out = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    ASSERT_EQ(backend.query_topology(1, 1, 2, &out), 0);
    ASSERT_EQ(out.net_layer_count, 1U);
    const auto* borrowed = out.net_layers;
    EXPECT_EQ(borrowed[0], ACLSHMEM_NET_LAYER_3);
    ASSERT_EQ(backend.query_topology(2, 1, 3, &out), 0);
    EXPECT_EQ(out.net_layer_count, 0U);
    EXPECT_EQ(out.net_layers, nullptr);
    EXPECT_EQ(borrowed[0], ACLSHMEM_NET_LAYER_3);
    // Copy survives owner release. Dereferencing the old borrowed pointer would be invalid.
    std::vector<aclshmemx_net_layer_t> copy(borrowed, borrowed + 1);
    first.topology_net_layers_by_pe.clear();
    first.topology_context.reset();
    EXPECT_EQ(copy, (std::vector<aclshmemx_net_layer_t>{ACLSHMEM_NET_LAYER_3}));
    EXPECT_EQ(exchange.calls, calls);
    backend.entity_map_.clear();
}
#endif
} // namespace
