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

    static int Gather(const void* send, void* receive, int bytes, aclshmemi_bootstrap_handle_t* handle)
    {
        auto& replay = *static_cast<Replay*>(handle->bootstrap_state);
        const int phase = ++replay.calls;
        // Production Initialize: agree/count/local-ID/length/agree/blob/agree.
        // Production build_topology_state: agree (phase 8).
        if (phase == 1 || phase == 5 || phase == 7 || phase == 8) {
            if (bytes != sizeof(int)) return -1;
            for (int pe = 0; pe < handle->npes; ++pe) {
                static_cast<int*>(receive)[pe] = *static_cast<const int*>(send);
            }
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
Replay PreparePeers(const std::vector<uint32_t>& devices, uint32_t source)
{
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
    for (size_t pe = 0; pe < devices.size(); ++pe) {
        std::cout << "[DEVICE] PE " << pe << " -> device_id " << devices[pe] << '\n';
    }
    Require(!devices.empty() && devices.size() <= ACLSHMEM_MAX_PES && source < devices.size(),
            "Invalid PE count or source");
    auto replay = PreparePeers(devices, source);
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
    const auto masks = spec.contains("internal_transport_masks")
        ? spec.at("internal_transport_masks").get<std::vector<uint32_t>>()
        : std::vector<uint32_t>(devices.size(), spec.value("internal_transport_mask", 1U));
    Require(masks.size() == devices.size(), "Wrong mask count");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        Require(masks[peer] <= 15, "Unknown internal transport bits");
        fixture.state.topo_list[peer] = static_cast<aclshmem_transport_t>(masks[peer]);
    }
    init_manager = &fixture.backend;
    g_instance_ctx = &fixture.instance;
    g_state.npes = fixture.state.npes;
    g_state.mype = fixture.state.mype;

    // REAL production build, not a precomputed layer injection.
    Require(fixture.backend.build_topology_state(fixture.instance.id) == ACLSHMEM_SUCCESS,
            "Production build_topology_state failed");
    Require(replay.calls == 8, "Missing build status agreement");
    aclshmemx_topology_t validation = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_NOT_INITED,
            "Query must reject uninitialized instance");
    g_state.is_aclshmem_initialized = true;
    Require(aclshmemx_query_topology(0, 3, nullptr) == ACLSHMEM_INVALID_PARAM, "Null output accepted");
    Require(aclshmemx_query_topology(-1, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Bad PE accepted");
    Require(aclshmemx_query_topology(0, 0, &validation) == ACLSHMEM_INVALID_VALUE, "Zero mask accepted");
    validation.struct_size = 63;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Short ABI accepted");

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
            Require(status == ACLSHMEM_SUCCESS, "Public query failed: " + std::to_string(status));
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
