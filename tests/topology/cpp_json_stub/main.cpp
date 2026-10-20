// TEMP_TOPOLOGY_CPP_JSON_STUB: portable fixture, not a production initialization entry.
// All topology parsing/matching, context discovery, snapshot build and queries are
// compiled from production C++ sources; only runtime/bootstrap/resource setup is stubbed.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include "init/shmemi_init.h"
#include "capability_fixture.h"

using Json = nlohmann::json;
using namespace shm::transport;
namespace fs = std::filesystem;

static int driverFallbackCalls = 0;
extern "C" int topo_addr_info_get_size(int, size_t*) { ++driverFallbackCalls; return -1; }
extern "C" int topo_addr_info_get(int, char*, size_t*) { ++driverFallbackCalls; return -1; }

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
Json Read(const fs::path& path)
{
    std::ifstream input(path);
    Require(input.good(), "Cannot open " + path.string());
    Json data;
    input >> data;
    return data;
}

struct Replay {
    std::vector<uint32_t> counts, ids, lengths;
    std::vector<std::vector<uint8_t>> blobs;
    int calls = 0;
    uint32_t source = 0;
    int gate_remote_status = 0;
    bool gate_pending = false;
    bool gate_transport_failure = false;

    static int Gather(const void* send, void* receive, int bytes, aclshmemi_bootstrap_handle_t* handle)
    {
        auto& replay = *static_cast<Replay*>(handle->bootstrap_state);
        const int phase = ++replay.calls;
        // Production Initialize: agree/count/local-ID/length/agree/blob/agree.
        // Production build_topology_state: agree (phase 8).
        if (phase == 1 || phase == 5 || phase == 7 || phase >= 8) {
            if (replay.gate_pending && replay.gate_transport_failure) return -1;
            if (bytes != sizeof(int)) return -1;
            for (int pe = 0; pe < handle->npes; ++pe) {
                static_cast<int*>(receive)[pe] = *static_cast<const int*>(send);
            }
            if (replay.gate_pending && replay.gate_remote_status != 0) {
                static_cast<int*>(receive)[(replay.source + 1) % handle->npes] = replay.gate_remote_status;
            }
            replay.gate_pending = false;
            return 0;
        }
        const std::vector<uint32_t>* values = nullptr;
        if (phase == 2) values = &replay.counts;
        if (phase == 3) values = &replay.ids;
        if (phase == 4) values = &replay.lengths;
        if (values) {
            if (bytes != sizeof(uint32_t) ||
                *static_cast<const uint32_t*>(send) != values->at(replay.source)) return -1;
            std::memcpy(receive, values->data(), values->size() * sizeof(uint32_t));
            return 0;
        }
        if (phase == 6) {
            const auto& own = replay.blobs.at(replay.source);
            if (bytes < 0 || static_cast<size_t>(bytes) < own.size() ||
                std::memcmp(send, own.data(), own.size()) != 0) return -1;
            std::memset(receive, 0, static_cast<size_t>(bytes) * replay.blobs.size());
            for (size_t pe = 0; pe < replay.blobs.size(); ++pe) {
                if (replay.blobs[pe].size() > static_cast<size_t>(bytes)) return -1;
                std::memcpy(static_cast<char*>(receive) + pe * bytes,
                            replay.blobs[pe].data(), replay.blobs[pe].size());
            }
            return 0;
        }
        return -1;
    }
};

// Construct the data that remote PEs would send, using the real C++ parsers and
// BuildLocalEndpoints. Never read expected_layers to create replay payloads.
Replay PreparePeers(const std::vector<uint32_t>& devices, uint32_t source, const std::string& input_key)
{
    // Cache only the immutable remote bootstrap input, never query results or
    // capability state. Every case still runs local discovery/build/query.
    static std::map<std::string, Replay> prepared;
    const auto cached = prepared.find(input_key);
    if (cached != prepared.end()) {
        auto replay = cached->second;
        replay.source = source;
        return replay;
    }
    Replay replay;
    replay.source = source;
    for (const auto device : devices) {
        RootInfo root;
        TopoInfo topo;
        uint32_t id = 0, count = 0;
        Require(TopoReader::ParseRootInfo(device, root), "Production ParseRootInfo failed");
        Require(TopoReader::ParseTopoInfo(root.topoFilePath, topo), "Production ParseTopoInfo failed");
        Require(TopoReader::GetLocalId(root, device, id), "GetLocalId failed");
        Require(TopoReader::GetEidCount(root, count), "GetEidCount failed");
        replay.ids.push_back(id);
        replay.counts.push_back(count);
        auto blob = Json::to_msgpack(Json(TopologyContext::BuildLocalEndpoints(root, topo)));
        replay.lengths.push_back(static_cast<uint32_t>(blob.size()));
        replay.blobs.push_back(std::move(blob));
    }
    prepared.emplace(input_key, replay);
    return replay;
}

struct Fixture {
    aclshmemi_init_backend backend;
    aclshmem_device_host_state_t state{};
    aclshmem_instance_ctx instance{23, nullptr};
    aclshmemx_init_attr_t attr{};
    aclshmemi_bootstrap_handle_t boot{};
    bool bound = false;
    ~Fixture()
    {
        if (bound) backend.release_aclshmem_entity(instance.id);
        init_manager = nullptr;
        g_instance_ctx = nullptr;
        g_state = {};
    }
};

void RunCase(const Json& spec, const fs::path& directory)
{
    const std::string name = spec.at("name");
    const auto rootPath = directory / spec.at("root_info").get<std::string>();
    auto input = Read(rootPath);
    // Resolve the referenced topology file locally; never invent physical edges.
    fs::path topologyPath;
    if (spec.contains("topology")) {
        topologyPath = directory / spec.at("topology").get<std::string>();
    } else {
        const fs::path described = input.at("topo_file_path").get<std::string>();
        topologyPath = described.is_absolute() ? described : rootPath.parent_path() / described;
        if (!fs::is_regular_file(topologyPath)) {
            topologyPath = rootPath.parent_path() / described.filename();
        }
    }
    Require(fs::is_regular_file(topologyPath),
            "Topology file missing: " + topologyPath.string() + "; supply --topology");
    input["topo_file_path"] = fs::canonical(topologyPath).string();
    {
        std::ofstream output(TopoReader::ROOTINFO_PATH);
        Require(output.good(), "Cannot create isolated RootInfo fixture");
        output << input.dump(2);
    }
    std::vector<uint32_t> devices;
    if (spec.contains("devices")) {
        devices = spec.at("devices").get<std::vector<uint32_t>>();
    } else {
        for (const auto& rank : input.at("rank_list")) {
            devices.push_back(rank.at("device_id").get<uint32_t>());
        }
        if (input.contains("rank_count")) {
            Require(input.at("rank_count").get<size_t>() == devices.size(), "rank_count mismatch");
        }
    }
    Require(std::set<uint32_t>(devices.begin(), devices.end()).size() == devices.size(),
            "Duplicate device IDs in RootInfo");
    const auto source = spec.value("my_pe", 0U);
    const bool acceptance = spec.value("acceptance_mode", false);
    for (size_t pe = 0; pe < devices.size(); ++pe) {
        std::cout << "[DEVICE] PE " << pe << " -> device_id " << devices[pe] << '\n';
    }
    Require(!devices.empty() && devices.size() <= ACLSHMEM_MAX_PES && source < devices.size(),
            "Invalid PE count or source");
    auto replay = PreparePeers(devices, source, input.dump() + Json(devices).dump() + Read(topologyPath).dump());
    Fixture fixture;
    fixture.boot.npes = static_cast<int>(devices.size());
    fixture.boot.mype = static_cast<int>(source);
    fixture.boot.bootstrap_state = &replay;
    fixture.boot.allgather = Replay::Gather;

    // REAL Initialize: calls mocked device-ID conversion, real ParseRootInfo/
    // ParseTopoInfo, real MessagePack encoding/decoding, and replay allgather.
    auto context = std::make_unique<TopologyContext>();
    Require(context->Initialize(static_cast<int32_t>(devices[source]), source,
                                static_cast<uint32_t>(devices.size()), &fixture.boot) == ACLSHMEM_SUCCESS,
            name + ": production TopologyContext::Initialize failed");
    Require(replay.calls == 7, "Unexpected discovery exchange order");
    Require(context->rank_to_local_id == replay.ids, "Remote local-ID exchange mismatch");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        Require(Json(context->rank_to_sync_endpoints[peer]) == Json::from_msgpack(replay.blobs[peer]),
                "Remote endpoint exchange mismatch");
    }

    fixture.attr.instance_id = fixture.instance.id;
    fixture.attr.n_pes = fixture.state.npes = fixture.boot.npes;
    fixture.attr.my_pe = fixture.state.mype = fixture.boot.mype;
    Require(fixture.backend.bind_aclshmem_entity(&fixture.attr, &fixture.state, &fixture.boot,
                nullptr, UdmaQpConfig{}, 1, 1) == 0, "Fixture bind failed");
    fixture.bound = true;
    fixture.backend.entity_map_.at(fixture.instance.id)->topology_context = std::move(context);
    const bool configurationMode = spec.contains("engine_config");
    Require(!configurationMode || (!spec.contains("internal_transport_mask") &&
            !spec.contains("internal_transport_masks")), "Cannot combine configuration and raw state injection");
    if (configurationMode) {
        const auto config = spec.at("engine_config").get<uint32_t>();
        Require((config & ~15U) == 0, "Unknown engine configuration bits");
        fixture.attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(config);
        const auto direct = spec.at("direct_reachable").get<std::vector<bool>>();
        Require(direct.size() == devices.size() && direct[source], "Explicit D per peer and reachable self required");
        shm::MemEntityDefault entity;
        entity.options_ = ConfigureEngines(&fixture.attr);
        entity.options_.rankId = source;
        shm::DlApi::load_status = spec.value("resource_status", 0);
        int init_status = entity.LoadExtendLibrary();
        shm::DlApi::load_status = 0;
        shm::RouteStub transport;
        auto route_layers = spec.value("udma_qp_layers", std::vector<std::vector<uint32_t>>(devices.size()));
        Require(route_layers.size() == devices.size(), "Wrong QP route count");
        if (spec.contains("route_qp_count")) {
            const auto& discovered = *fixture.backend.entity_map_.at(fixture.instance.id)->topology_context;
            std::map<uint32_t, uint32_t> policy;
            for (const auto& item : spec.value("qp_policy", Json::array())) {
                policy[item.at(0).get<uint32_t>()] = item.at(1).get<uint32_t>();
            }
            TopoQuerier planner(discovered.root_info, discovered.topo_info, source,
                discovered.rank_to_local_id, discovered.rank_to_sync_endpoints, policy,
                spec.value("multi_link", false));
            const auto count = spec.at("route_qp_count").get<uint32_t>();
            Require(count > 0 && count <= 4, "Acceptance route count must be 1/2/4");
            for (size_t peer = 0; peer < devices.size(); ++peer) {
                std::vector<EidRoute> routes;
                const int status = planner.GetEidRoutes(static_cast<uint32_t>(peer), count, routes);
                route_layers[peer].clear();
                if (status == ACLSHMEM_SUCCESS) for (const auto& route : routes) {
                    route_layers[peer].push_back(route.netLayer);
                }
                if (acceptance) std::cout << Json{{"record_type", "qp_route"}, {"case", name},
                    {"platform", TOPOLOGY_TEST_PLATFORM}, {"source_pe", source}, {"target_pe", peer},
                    {"requested_qp_count", count}, {"route_status", status},
                    {"resolved_layers", route_layers[peer]}, {"hardware_qp_created", false}}.dump() << '\n';
            }
        }
        for (size_t peer = 0; peer < route_layers.size(); ++peer) {
            for (const auto layer : route_layers[peer]) transport.peer_routes_[peer].qp_routes.push_back({layer});
        }
        if (spec.value("transport_manager", true)) entity.transportManager_ = &transport;
        for (size_t peer = 0; peer < devices.size(); ++peer) {
            entity.direct = direct[peer];
            const auto capability = entity.CanReachDataOperators(static_cast<uint32_t>(peer));
            StoreCapability(&fixture.state, static_cast<int32_t>(peer), capability);
        }
        replay.gate_remote_status = spec.value("remote_init_status", 0);
        replay.gate_transport_failure = spec.value("gate_transport_failure", false);
        replay.gate_pending = true;
        if (init_status == ACLSHMEM_SUCCESS) {
            init_status = fixture.backend.validate_peer_reachability(fixture.instance.id);
        }
        Require(init_status == spec.value("expected_init_status", 0), name + ": init-stage status mismatch");
        if (acceptance) {
            std::cout << Json{{"record_type", "init_stage"}, {"case", name},
                {"platform", TOPOLOGY_TEST_PLATFORM}, {"engine_config", config},
                {"stage_status", init_status}, {"full_device_init", false},
                {"direct_reachable", direct}, {"udma_qp_layers", route_layers}}.dump() << '\n';
        }
        if (init_status != ACLSHMEM_SUCCESS) {
            init_manager = &fixture.backend;
            g_instance_ctx = &fixture.instance;
            g_state.npes = fixture.state.npes;
            aclshmemx_topology_t output = ACLSHMEMX_TOPOLOGY_INITIALIZER;
            Require(aclshmemx_query_topology(0, 1, &output) == ACLSHMEM_NOT_INITED,
                "Failed init stage must not publish a queryable instance");
            return;
        }
    }
    const auto masks = spec.contains("internal_transport_masks")
        ? spec.at("internal_transport_masks").get<std::vector<uint32_t>>()
        : std::vector<uint32_t>(devices.size(), spec.value("internal_transport_mask", 1U));
    Require(masks.size() == devices.size(), "Wrong mask count");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        Require(masks[peer] <= 15, "Unknown internal transport bits");
        if (!configurationMode) fixture.state.topo_list[peer] = static_cast<aclshmem_transport_t>(masks[peer]);
    }
    init_manager = &fixture.backend;
    g_instance_ctx = &fixture.instance;
    g_state.npes = fixture.state.npes;
    g_state.mype = fixture.state.mype;

    // REAL production build, not a precomputed layer injection.
    Require(fixture.backend.build_topology_state(fixture.instance.id) == ACLSHMEM_SUCCESS,
            "Production build_topology_state failed");
#ifdef ACLSHMEM_SOC_950
    Require(replay.calls == (configurationMode ? 9 : 8), "Missing build status agreement");
#else
    Require(replay.calls == (configurationMode ? 8 : 7), "Unexpected non-950 cache exchange");
#endif
    aclshmemx_topology_t validation = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_NOT_INITED,
            "Query must reject uninitialized instance");
    g_state.is_aclshmem_initialized = true;
    Require(aclshmemx_query_topology(0, 3, nullptr) == ACLSHMEM_INVALID_PARAM, "Null output accepted");
    Require(aclshmemx_query_topology(-1, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Bad PE accepted");
    Require(aclshmemx_query_topology(0, 0, &validation) == ACLSHMEM_INVALID_VALUE, "Zero mask accepted");
    validation.struct_size = 63;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Short ABI accepted");
    if (acceptance) {
        auto check_error = [&](const char* label, int peer, uint32_t mask, int expected,
                               aclshmemx_topology_t* output) {
            const int actual = aclshmemx_query_topology(peer, mask, output);
            Require(actual == expected, std::string("Validation mismatch: ") + label);
            std::cout << Json{{"record_type", "validation"}, {"case", name},
                {"platform", TOPOLOGY_TEST_PLATFORM}, {"check", label},
                {"query_mask", mask}, {"status", actual}, {"expected_status", expected}}.dump() << '\n';
        };
        aclshmemx_topology_t out = ACLSHMEMX_TOPOLOGY_INITIALIZER;
        g_state.is_aclshmem_initialized = false;
        check_error("not_initialized", 0, 1, ACLSHMEM_NOT_INITED, &out);
        g_state.is_aclshmem_initialized = true;
        check_error("null_result", 0, 1, ACLSHMEM_INVALID_PARAM, nullptr);
        check_error("negative_pe", -1, 1, ACLSHMEM_INVALID_VALUE, &out);
        check_error("pe_too_large", fixture.state.npes, 1, ACLSHMEM_INVALID_VALUE, &out);
        for (uint32_t mask : {0U, 4U, 5U, 0xffffffffU})
            check_error("invalid_mask", 0, mask, ACLSHMEM_INVALID_VALUE, &out);
        out.struct_size = 63;
        check_error("short_layout", 0, 1, ACLSHMEM_INVALID_VALUE, &out);
        out.struct_size = sizeof(out);
        out.abi_version = 2;
        check_error("wrong_abi", 0, 1, ACLSHMEM_INVALID_VALUE, &out);
    }

    const bool checkEngines = spec.contains("expected_engine_masks");
    const bool checkLayers = spec.contains("expected_layers");
    const auto expectedEngines = checkEngines
        ? spec.at("expected_engine_masks").get<std::vector<uint32_t>>() : std::vector<uint32_t>{};
    const auto expectedLayers = checkLayers
        ? spec.at("expected_layers").get<std::vector<std::vector<uint32_t>>>()
        : std::vector<std::vector<uint32_t>>{};
    Require((!checkEngines || expectedEngines.size() == devices.size()) &&
            (!checkLayers || expectedLayers.size() == devices.size()), "Wrong expectation count");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        for (uint32_t mask : {1U, 2U, 3U}) {
            aclshmemx_topology_t output = ACLSHMEMX_TOPOLOGY_INITIALIZER;
            output.reserved[0] = 0x12345678U;
            // REAL public C++ function body from shmem_init.cpp.
            const int status = aclshmemx_query_topology(static_cast<int>(peer), mask, &output);
            int expectedStatus = ACLSHMEM_SUCCESS;
#ifndef ACLSHMEM_SOC_950
            if (mask & ACLSHMEMX_TOPOLOGY_QUERY_LINKS) expectedStatus = ACLSHMEM_NOT_SUPPORTED;
#endif
            Require(status == expectedStatus, "Public query status mismatch: " + std::to_string(status));
            if (status != ACLSHMEM_SUCCESS) {
                std::cout << Json{{"record_type", "query"}, {"case", name},
                    {"platform", TOPOLOGY_TEST_PLATFORM}, {"source_pe", source}, {"target_pe", peer},
                    {"query_mask", mask}, {"status", status}}.dump() << '\n';
                continue;
            }
            std::vector<uint32_t> layers;
            for (uint32_t i = 0; i < output.net_layer_count; ++i) {
                layers.push_back(static_cast<uint32_t>(output.net_layers[i]));
            }
            const uint32_t expectedEngine = mask & 1U && checkEngines ? expectedEngines[peer] : 0U;
            const auto expectedLayer = mask & 2U && checkLayers ? expectedLayers[peer] : std::vector<uint32_t>{};
            Require((!checkEngines && (mask & 1U)) || output.engine_mask == expectedEngine, name + ": ENGINES mismatch at PE " + std::to_string(peer) +
                    " actual=" + std::to_string(output.engine_mask) + " expected=" + std::to_string(expectedEngine));
            Require((!checkLayers && (mask & 2U)) || layers == expectedLayer, name + ": LINKS mismatch at PE " + std::to_string(peer) +
                    " actual=" + Json(layers).dump() + " expected=" + Json(expectedLayer).dump());
            Require(output.reserved[0] == 0x12345678U, "Query changed reserved field");
            Require(!layers.empty() || output.net_layers == nullptr, "Empty layers must use nullptr");
            std::cout << Json{{"mode", "production_cpp"}, {"case", name}, {"source_pe", source},
                              {"record_type", "query"}, {"platform", TOPOLOGY_TEST_PLATFORM},
                              {"engine_state_source", configurationMode ? "production_capability_with_stub_D" : "raw_transport_injection"},
                              {"engine_config", configurationMode ? spec.at("engine_config") : Json(nullptr)},
                              {"target_pe", peer}, {"target_device_id", devices[peer]},
                              {"source_device_id", devices[source]},
                              {"engine_expectation_checked", checkEngines}, {"links_expectation_checked", checkLayers},
                              {"query_mask", mask}, {"status", status},
                              {"engine_mask", output.engine_mask}, {"net_layer_count", output.net_layer_count},
                              {"net_layers", layers}}.dump() << '\n';
        }
    }
    Require(driverFallbackCalls == 0, "Unexpected hardware rootinfo fallback");
    std::cout << "[CHECK] " << name << ": engines expected=" << checkEngines
              << ", links expected=" << checkLayers << " (0 = observation only)\n";
}

int main(int argc, char** argv)
{
    try {
        Require(argc == 2 || argc == 3, "Usage: cpp-stub cases.json [case-name]");
        const fs::path path = fs::absolute(argv[1]);
        const auto suite = Read(path);
        int passed = 0;
        for (const auto& spec : suite.at("cases")) {
            if (argc == 3 && spec.at("name").get<std::string>() != argv[2]) continue;
            RunCase(spec, path.parent_path());
            ++passed;
        }
        Require(passed > 0, "No matching cases");
        std::cout << "PASS: " << passed << " C++ JSON cases (real public query, ASan+UBSan)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
