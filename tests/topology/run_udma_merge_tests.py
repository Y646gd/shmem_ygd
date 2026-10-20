#!/usr/bin/env python3
"""CPU integration checks for the 1019 UDMA/context merge.

Extracts production OpenDevice, PrepareOpenDevice and EndpointQpSlot, compiles the real
router/context. Device identity, bootstrap and endpoint creation are explicit
stubs; no hardware QP/channel or full CANN object layout is claimed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json', type=Path, default=Path('/tmp/shmem-rfc-json-v3.11.3'))
args = parser.parse_args()


def function(path, name):
    source = (ROOT/path).read_text()
    start = source.index(name)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


with tempfile.TemporaryDirectory(prefix='shmem1019-udma-merge-') as folder:
    temp = Path(folder)
    inc = temp/'include'
    def stub(name, source):
        path = inc/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
    stub('acl/acl_rt.h', '#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    stub('shmemi_host_common.h', r'''#pragma once
#include "host/shmem_host_def.h"
#include <sstream>
#define SHM_LOG_ERROR(m) do { std::ostringstream s; s << m; } while (0)
#define SHM_LOG_INFO(m) SHM_LOG_ERROR(m)
#define SHM_LOG_DEBUG(m) SHM_LOG_ERROR(m)
#define SHM_LOG_WARN(m) SHM_LOG_ERROR(m)
''')
    stub('shmemi_file_util.h', r'''#pragma once
#include <filesystem>
#include <string>
namespace shm::utils { struct FileUtil {
static bool IsSymlink(const std::string& p) { return std::filesystem::is_symlink(p); }
static bool Realpath(std::string&) { return true; }
static bool IsFile(const std::string& p) { return std::filesystem::is_regular_file(p); }
static bool CheckFileSize(const std::string& p) { return std::filesystem::file_size(p) < 10*1024*1024; }
}; }
''')
    stub('dl_acl_api.h', r'''#pragma once
#include <cstdint>
namespace shm { struct DlAclApi {
inline static int get_device_calls = 0, get_device_status = 0;
static int AclrtGetDevice(int32_t* out) { ++get_device_calls; *out=0; return get_device_status; }
static int RtGetLogicDevIdByUserDevId(int32_t x, int32_t* y) { *y=x; return 0; }
static int AclrtGetPhyDevIdByLogicDevId(int32_t x, int32_t* y) { *y=x; return 0; }
}; }
''')
    manager_header = (ROOT/'src/host/transport/device_udma/device_udma_transport_manager.h').read_text()
    begin = manager_header.index('    struct QpRoute {')
    end = manager_header.index('    // Per-fill bookkeeping', begin)
    source = r'''
#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include "transport/topo/topo_reader.h"
#include "utils/shmemi_host_types.h"
#include "shmemi_host_common.h"
#include "dl_acl_api.h"
#include "mem/heap/hybm_def.h"
std::string discovery_root;
extern "C" int topo_addr_info_get_size(int, size_t* size) {
    if (discovery_root.empty()) return -1;
    *size = discovery_root.size()+1; return 0;
}
extern "C" int topo_addr_info_get(int, char* data, size_t* size) {
    if (*size < discovery_root.size()+1) return -1;
    std::memcpy(data, discovery_root.c_str(), discovery_root.size()+1); return 0;
}
aclshmemi_bootstrap_handle_t g_boot_handle{};
namespace shm {
struct MemSegment {
    inline static int status = 0;
    static int GetDeviceInfo(uint32_t& sd, uint32_t& server, uint32_t& pod) {
        sd=0; server=10; pod=1; return status;
    }
};
namespace transport {
struct TransportOptions {
    uint32_t rankId = 0, rankCount = 2;
    hybm_role_type role = hybm_role_type::HYBM_ROLE_PEER;
    struct { uint32_t qpNum = 1; bool sharedJettyEnabled = false;
             std::map<uint32_t, uint32_t> netLayerPolicy; bool multiLinkEnabled = false; } udmaQpConfig;
    const TopologyContext* topologyContext = nullptr;
};
}
namespace transport::device {
using Result = int;
constexpr bool ACLSHMEM_UDMA_RELAY_ENABLED = false;
constexpr int32_t INVALID_EID_INDEX = -1;
struct UdmaTransportManager {
    const TopologyContext* topology_context_ = nullptr;
    std::unique_ptr<TopologyContext> owned_topology_context_;
    hybm_role_type role_ = hybm_role_type::HYBM_ROLE_PEER;
    uint32_t phy_id_ = 0, rank_id_ = 0, rank_count_ = 2, qp_num_ = 1;
    bool multi_link_enabled_ = false, shared_jetty_enabled_ = false;
    bool endpoint_success = true;
    std::map<uint32_t, uint32_t> net_layer_policy_;
    std::vector<int32_t> all_local_routes_;
    struct EndpointKey { uint32_t eid_index; uint32_t qp_slot; };
    std::vector<EndpointKey> created;
''' + manager_header[begin:end] + r'''
    std::map<uint32_t, PeerRouteInfo> peer_routes_;
    bool CreateEndpoint(const EndpointKey& key, const EidData&) {
        created.push_back(key); return endpoint_success;
    }
    Result PrepareOpenDevice(uint32_t);
    Result OpenDevice(const TransportOptions&);
''' + function('src/host/transport/device_udma/device_udma_transport_manager.h',
               'uint32_t EndpointQpSlot(') + '\n};\n'
    source += function('src/host/transport/device_udma/device_udma_transport_manager.cpp',
                       'Result UdmaTransportManager::OpenDevice(')
    source += function('src/host/transport/device_udma/device_udma_transport_manager.cpp',
                       'Result UdmaTransportManager::PrepareOpenDevice(')
    source += r'''
} } // namespace shm::transport::device, shm
struct entity_member {
    aclshmem_device_host_state_t* entity_host_state = nullptr;
    std::unique_ptr<shm::transport::TopologyContext> topology_context;
};
struct aclshmemi_init_backend {
    std::mutex entity_map_mutex_;
    std::map<uint64_t, entity_member*> entity_map_;
    int install_topology_context_for_test(uint64_t, std::unique_ptr<shm::transport::TopologyContext>);
};
'''
    source += function('src/host/init/backends/shmem_init_backend.cpp',
                       'int aclshmemi_init_backend::install_topology_context_for_test(')
    source += r'''
using namespace shm::transport;
using shm::transport::device::UdmaTransportManager;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Exchange {
    uint32_t remote_server = 10;
    int calls = 0, fail_at = 0, remote_error_at = 0;
    static int Gather(const void* in, void* out, int bytes, aclshmemi_bootstrap_handle_t* boot) {
        auto& x = *static_cast<Exchange*>(boot->bootstrap_state);
        ++x.calls;
        if (x.calls == x.fail_at) return -1;
        Require(bytes == 4, "unexpected exchange width");
        std::memcpy(out, in, bytes);
        if (x.calls == 3) static_cast<uint32_t*>(out)[1] = x.remote_server;
        else static_cast<int*>(out)[1] = x.calls == x.remote_error_at ? ACLSHMEM_MALLOC_FAILED : 0;
        return 0;
    }
};
TopologyContext Context() {
    TopologyContext c;
    c.my_pe = 0; c.physical_device_id = 7; c.max_eid_slot_count = 2;
    c.rank_to_local_id = {7, 3};
    c.rank_to_sync_endpoints.resize(2);
    for (uint32_t rank = 0; rank < 2; ++rank) {
        SyncEndpoint ep{0, "domain", NetType::Mesh, TopoFabric::Mesh, "plane", {}, rank};
        ep.ports = {rank ? "b" : "a"};
        c.rank_to_sync_endpoints[rank].push_back(ep);
        if (rank == 0) {
            auto addr = std::make_shared<RankAddr>();
            addr->eidData[15] = 1;
            addr->ports = ep.ports;
            c.root_info.eidIndexToRankAddr[0] = addr;
            c.root_info.portsToRankAddr["a"] = addr;
        }
    }
    c.topo_info.meshTopoEdges.push_back({7, 3, {"a"}, {"b"}});
    return c;
}
int main() {
    try {
        Exchange exchange;
        g_boot_handle.npes = 2;
        g_boot_handle.bootstrap_state = &exchange;
        g_boot_handle.allgather = Exchange::Gather;
        auto context = Context();
        auto manager = [&]() {
            exchange = {}; shm::MemSegment::status = 0;
            UdmaTransportManager m; m.topology_context_ = &context; return m;
        };
        auto m = manager();
        Require(m.PrepareOpenDevice(2) == 0, "same-server route failed");
        Require(m.created.size() == 1 && m.peer_routes_.count(1) == 1, "missing route/endpoint");
        Require(m.phy_id_ == 7 && exchange.calls == 3, "context identity / discovery reuse");
        m = manager(); exchange.remote_server = 20;
        Require(m.PrepareOpenDevice(2) == 0 && m.created.empty() && m.peer_routes_.empty(),
                "cross-server peer must skip UDMA successfully");
        m = manager(); context.topo_info.meshTopoEdges.clear();
        Require(m.PrepareOpenDevice(2) == 0 && m.created.empty(), "absent route must skip UDMA");
        context = Context();
        m = manager(); m.endpoint_success = false;
        Require(m.PrepareOpenDevice(2) == ACLSHMEM_INNER_ERROR, "endpoint error silently fell back");
        m = manager(); m.shared_jetty_enabled_ = true; m.qp_num_ = 2;
        Require(m.PrepareOpenDevice(2) == 0 && m.created.size() == 2, "multi-QP route preparation");
        Require(m.created[0].qp_slot == 0 && m.created[1].qp_slot == 1, "shared jetty QP slots merged");
        m = manager(); m.qp_num_ = 2;
        Require(m.PrepareOpenDevice(2) == 0 && m.created[1].qp_slot == 0, "default endpoint slot changed");
        m = manager(); shm::MemSegment::status = -9;
        Require(m.PrepareOpenDevice(2) == -9 && exchange.calls == 1, "identity error not agreed");
        for (int phase = 1; phase <= 3; ++phase) {
            m = manager(); exchange.fail_at = phase;
            Require(m.PrepareOpenDevice(2) == ACLSHMEM_BOOTSTRAP_ERROR && exchange.calls == phase,
                    "bootstrap failure did not stop");
        }
        m = manager(); exchange.remote_error_at = 1;
        Require(m.PrepareOpenDevice(2) == ACLSHMEM_MALLOC_FAILED && exchange.calls == 1,
                "remote failure not propagated");
        m = manager(); m.topology_context_ = nullptr;
        Require(m.PrepareOpenDevice(2) == ACLSHMEM_NOT_INITED && exchange.calls == 0, "null context accepted");
        aclshmemi_init_backend backend;
        Require(backend.install_topology_context_for_test(23, nullptr) == ACLSHMEM_NOT_INITED,
                "unknown injection instance accepted");
        aclshmem_device_host_state_t host{}; host.npes=2; host.mype=0;
        entity_member entity; entity.entity_host_state = &host; backend.entity_map_[23] = &entity;
        Require(backend.install_topology_context_for_test(23, nullptr) == ACLSHMEM_INVALID_VALUE,
                "null injection accepted");
        auto wrong = std::make_unique<TopologyContext>(Context()); wrong->my_pe = 1;
        Require(backend.install_topology_context_for_test(23, std::move(wrong)) == ACLSHMEM_INVALID_VALUE,
                "wrong PE injection accepted");
        auto injected = std::make_unique<TopologyContext>(Context());
        auto* original = injected.get();
        Require(backend.install_topology_context_for_test(23, std::move(injected)) == 0 &&
                !injected && entity.topology_context.get() == original, "context ownership not transferred");
        // RFC 4.4: supplied context is borrowed without rerunning runtime discovery.
        m = manager(); shm::DlAclApi::get_device_calls = 0;
        TransportOptions options; options.topologyContext = &context;
        options.udmaQpConfig.qpNum = 2; options.udmaQpConfig.sharedJettyEnabled = true;
        Require(m.OpenDevice(options) == 0 && m.topology_context_ == &context &&
                !m.owned_topology_context_ && shm::DlAclApi::get_device_calls == 0 && exchange.calls == 3,
                "borrowed context was rediscovered or ownership changed");
        Require(m.shared_jetty_enabled_ && m.created[1].qp_slot == 1, "shared jetty option lost");
        m = manager(); options.udmaQpConfig.qpNum = 0;
        Require(m.OpenDevice(options) == ACLSHMEM_INVALID_VALUE && exchange.calls == 0, "zero QP accepted");
        m = manager(); options.udmaQpConfig.qpNum = ACLSHMEM_MAX_QP_NUM + 1;
        Require(m.OpenDevice(options) == ACLSHMEM_INVALID_VALUE && exchange.calls == 0, "too many QPs accepted");
        m = manager(); options.udmaQpConfig.qpNum = 1; options.rankId = 1;
        Require(m.OpenDevice(options) == ACLSHMEM_INVALID_VALUE && exchange.calls == 0, "foreign context PE accepted");
        m = manager(); options.rankId = 0; options.topologyContext = nullptr;
        shm::DlAclApi::get_device_status = -9;
        Require(m.OpenDevice(options) == ACLSHMEM_DL_FUNC_FAILED && exchange.calls == 2,
                "owned-context runtime failure did not agree");
        shm::DlAclApi::get_device_status = 0;
        // Successful standalone HYBM path uses the same real Initialize builder.
        struct OwnedExchange {
            int calls = 0;
            static int Gather(const void* in, void* out, int bytes, aclshmemi_bootstrap_handle_t* boot) {
                ++*static_cast<int*>(boot->bootstrap_state);
                for (int pe = 0; pe < boot->npes; ++pe)
                    std::memcpy(static_cast<char*>(out)+pe*bytes, in, bytes);
                return 0;
            }
        };
        m = manager(); int owned_calls = 0;
        g_boot_handle.bootstrap_state = &owned_calls; g_boot_handle.allgather = OwnedExchange::Gather;
        discovery_root = nlohmann::json{{"topo_file_path", TEST_TOPO_PATH}, {"rank_list", {
            {{"device_id", 0}, {"local_id", 0}, {"level_list", {
                {{"net_layer", 3}, {"net_type", "CLOS"}, {"net_instance_id", "fabric"},
                 {"rank_addr_list", {{{"addr_type", "IPV4"}, {"addr", "192.0.2.1"},
                                      {"ports", {"d2h"}}, {"plane_id", "plane"}}}}}
            }}}
        }}}.dump();
        Require(m.OpenDevice(options) == 0 && m.owned_topology_context_ &&
                m.topology_context_ == m.owned_topology_context_.get() && owned_calls == 12 && m.created.empty(),
                "standalone context builder / no-route fallback failed");
        std::cout << "PASS: 12 UDMA merge checks + 4 context injection checks + 6 OpenDevice checks (CPU stubs)\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
'''
    (temp/'topo.json').write_text('{"edge_list":[{"local_a":0,"local_b":1,"local_a_ports":["a"],"local_b_ports":["b"]}]}')
    source = '#define TEST_TOPO_PATH '+__import__('json').dumps(str(temp/'topo.json'))+'\n'+source
    path = temp/'main.cpp'
    path.write_text(source)
    includes = [inc, ROOT/'include', ROOT/'src/host', ROOT/'src/host/transport',
                ROOT/'src/host/bootstrap/config_store', args.json/'single_include']
    binary = temp/'udma-merge'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-fsanitize=address,undefined', '-pthread',
                    '-DACLSHMEM_SOC_950', *['-I'+str(p) for p in includes], str(path),
                    str(ROOT/'src/host/transport/topo/topo_reader.cpp'),
                    str(ROOT/'src/host/transport/topo/topology_context.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
