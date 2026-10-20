/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
// TEMP_TOPOLOGY_JSON_STUB: delete this test and its two guarded hooks together.
#include <gtest/gtest.h>
#if defined(ACLSHMEM_UNIT_TEST) && defined(ACLSHMEM_SOC_950)
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "host/init/shmem_host_init.h"
#include "init/shmemi_init.h"
#include "transport/topo/topo_reader.h"
#include "transport_def.h"

namespace {
using shm::transport::TopologyContext;
using shm::transport::TopoReader;
using Json = nlohmann::json;

std::string Parent(const std::string& path)
{
    const auto pos = path.find_last_of('/');
    return pos == std::string::npos ? "." : path.substr(0, pos);
}
std::string Resolve(const std::string& directory, const std::string& path)
{
    return !path.empty() && path[0] == '/' ? path : directory + "/" + path;
}
Json ReadJson(const std::string& path)
{
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open JSON fixture: " + path);
    Json value;
    file >> value;
    return value;
}

// Replaces runtime discovery and allgather with all ranks' JSON descriptions.
// Both JSON parsers, endpoint builder, layer matcher and public query stay real.
std::unique_ptr<TopologyContext> LoadTopologyJsonStub(
    const Json& spec, const std::string& directory)
{
    const auto devices = spec.at("devices").get<std::vector<uint32_t>>();
    const auto rank = spec.at("my_pe").get<uint32_t>();
    if (devices.empty() || devices.size() > ACLSHMEM_MAX_PES || rank >= devices.size()) {
        throw std::runtime_error("Invalid fixture PE count or source PE");
    }
    auto rootJson = ReadJson(Resolve(directory, spec.at("root_info").get<std::string>()));
    // Resolve fixtures relative to cases.json, never depend on process cwd.
    const auto topoPath = Resolve(directory, spec.at("topology").get<std::string>());
    auto context = std::make_unique<TopologyContext>();
    context->my_pe = rank;
    context->physical_device_id = devices[rank];
    for (size_t pe = 0; pe < devices.size(); ++pe) {
        shm::transport::RootInfo root;
        shm::transport::TopoInfo topo;
        if (!TopoReader::ParseRootInfoJsonForTest(rootJson, devices[pe], root)) {
            throw std::runtime_error("RootInfo parse rejected fixture");
        }
        root.topoFilePath = topoPath;
        uint32_t localId = 0, count = 0;
        if (!TopoReader::ParseTopoInfo(root.topoFilePath, topo) ||
            !TopoReader::GetLocalId(root, devices[pe], localId) ||
            !TopoReader::GetEidCount(root, count)) {
            throw std::runtime_error("Topology/local ID/EID count parse rejected fixture");
        }
        context->rank_to_local_id.push_back(localId);
        context->rank_to_sync_endpoints.push_back(TopologyContext::BuildLocalEndpoints(root, topo));
        context->max_eid_slot_count = std::max(context->max_eid_slot_count, count);
        if (pe == rank) {
            context->root_info = std::move(root);
            context->topo_info = std::move(topo);
        }
    }
    return context;
}

class TopologyJsonStubTest : public testing::Test {
protected:
    aclshmemi_init_backend backend;
    aclshmemx_init_attr_t attr{};
    aclshmem_device_host_state_t state{};
    aclshmem_instance_ctx instance{23, nullptr};
    aclshmemi_bootstrap_handle_t boot{};
    aclshmemi_init_backend* savedManager = init_manager;
    aclshmem_instance_ctx* savedInstance = g_instance_ctx;
    decltype(g_state) savedState = g_state;
    bool bound = false;

    void TearDown() override
    {
        if (bound) backend.release_aclshmem_entity(instance.id);
        init_manager = savedManager;
        g_instance_ctx = savedInstance;
        g_state = savedState;
    }

    void RunCase(const Json& spec, const std::string& directory)
    {
        SCOPED_TRACE(spec.at("name").get<std::string>());
        if (!spec.at("parse_ok").get<bool>()) {
            EXPECT_THROW(LoadTopologyJsonStub(spec, directory), std::runtime_error);
            return;
        }
        auto context = LoadTopologyJsonStub(spec, directory);
        const auto npes = context->rank_to_local_id.size();
        const auto masks = spec.at("internal_transport_masks").get<std::vector<uint32_t>>();
        const auto expectedEngines = spec.at("expected_engine_masks").get<std::vector<uint32_t>>();
        const auto expectedLayers = spec.at("expected_layers").get<std::vector<std::vector<uint32_t>>>();
        ASSERT_EQ(masks.size(), npes);
        ASSERT_EQ(expectedEngines.size(), npes);
        ASSERT_EQ(expectedLayers.size(), npes);
        attr.instance_id = instance.id;
        attr.n_pes = static_cast<int>(npes);
        attr.my_pe = static_cast<int>(context->my_pe);
        state.npes = attr.n_pes;
        state.mype = attr.my_pe;
        boot.npes = attr.n_pes;
        // build_topology_state only exchanges status here. Simulate successful peers.
        boot.allgather = [](const void* in, void* out, int bytes, aclshmemi_bootstrap_handle_t* handle) {
            if (bytes != sizeof(int)) return -1;
            for (int pe = 0; pe < handle->npes; ++pe) {
                static_cast<int*>(out)[pe] = *static_cast<const int*>(in);
            }
            return 0;
        };
        ASSERT_EQ(backend.bind_aclshmem_entity(&attr, &state, &boot, nullptr,
            shm::transport::UdmaQpConfig{}, 1, 1), ACLSHMEM_SUCCESS);
        bound = true;
        init_manager = &backend;
        g_instance_ctx = &instance;
        g_state.npes = state.npes;
        g_state.mype = state.mype;
        g_state.is_aclshmem_initialized = false;
        for (size_t pe = 0; pe < npes; ++pe) {
            ASSERT_LE(masks[pe], 15U);
            state.topo_list[pe] = static_cast<aclshmem_transport_t>(masks[pe]);
        }
        ASSERT_EQ(backend.install_topology_context_for_test(instance.id, std::move(context)), ACLSHMEM_SUCCESS);
        ASSERT_EQ(backend.build_topology_state(instance.id), ACLSHMEM_SUCCESS);
        g_state.is_aclshmem_initialized = true;
        for (int peer = 0; peer < state.npes; ++peer) {
            for (uint32_t mask : {1U, 2U, 3U}) {
                aclshmemx_topology_t result = ACLSHMEMX_TOPOLOGY_INITIALIZER;
                ASSERT_EQ(aclshmemx_query_topology(peer, mask, &result), ACLSHMEM_SUCCESS);
                EXPECT_EQ(result.engine_mask, mask & 1U ? expectedEngines[peer] : 0U);
                std::vector<uint32_t> layers;
                for (uint32_t i = 0; i < result.net_layer_count; ++i) {
                    layers.push_back(static_cast<uint32_t>(result.net_layers[i]));
                }
                EXPECT_EQ(layers, mask & 2U ? expectedLayers[peer] : std::vector<uint32_t>{});
                if (layers.empty()) EXPECT_EQ(result.net_layers, nullptr);
                std::cout << "[JSON_STUB] " << spec.at("name").get<std::string>()
                          << " PE " << state.mype << " -> " << peer << " query=" << mask
                          << " engines=" << result.engine_mask << " layers=" << Json(layers).dump() << '\n';
            }
        }
    }
};

TEST_F(TopologyJsonStubTest, ReadsJsonAndQueriesRealPublicApi)
{
    const char* custom = std::getenv("ACLSHMEM_TOPOLOGY_FIXTURE");
    const std::string path = custom ? custom :
        Parent(__FILE__) + "/../../../topology/json_stub/cases.json";
    ASSERT_NO_THROW({
        const auto suite = ReadJson(path);
        ASSERT_FALSE(suite.at("cases").empty());
        for (const auto& spec : suite.at("cases")) {
            RunCase(spec, Parent(path));
            if (bound) {
                backend.release_aclshmem_entity(instance.id);
                bound = false;
            }
            if (HasFatalFailure()) break;
        }
    });
}
} // namespace
#endif
