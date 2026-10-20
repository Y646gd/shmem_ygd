#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Portable topology tests. Requires local json 3.11.3 and GoogleTest 1.14 sources.

Compiles the real topo_reader.cpp and topology_context.cpp. Runtime/file utility
interfaces are mocked. Host query bodies are extracted verbatim from production
sources and compiled against a minimal backend fixture; this is not a full CANN
build or a transport/hardware test. ASan/UBSan are enabled.
"""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json', required=True, type=Path, help='nlohmann/json checkout')
parser.add_argument('--gtest', required=True, type=Path, help='googletest checkout')
parser.add_argument('--platforms', nargs='+', choices=['910', 'A2', 'A3', '950'], default=['910', '950'])
parser.add_argument('--output', type=Path, help='Retain GoogleTest XML and compiled-source fingerprints')
args = parser.parse_args()


def function(path, name):
    source = (ROOT / path).read_text()
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


with tempfile.TemporaryDirectory(prefix='shmem-rfc-tests-') as folder:
    temp = Path(folder)
    inc = temp / 'include'
    def stub(name, content):
        path = inc / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    stub('acl/acl_rt.h', '#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    stub('shmemi_host_common.h', '''#pragma once
#include "host/shmem_host_def.h"
#include <sstream>
#define SHM_LOG_ERROR(m) do { std::ostringstream s; s << m; } while (0)
#define SHM_LOG_WARN(m) SHM_LOG_ERROR(m)
#define SHM_LOG_INFO(m) SHM_LOG_ERROR(m)
#define SHM_LOG_DEBUG(m) SHM_LOG_ERROR(m)
''')
    stub('shmemi_file_util.h', '''#pragma once
#include <filesystem>
#include <string>
namespace shm::utils { struct FileUtil {
static bool IsSymlink(const std::string& p) { return std::filesystem::is_symlink(p); }
static bool Realpath(std::string&) { return true; }
static bool IsFile(const std::string& p) { return std::filesystem::is_regular_file(p); }
static bool CheckFileSize(const std::string& p) { return std::filesystem::file_size(p) < 10*1024*1024; }
}; }
''')
    stub('dl_acl_api.h', '''#pragma once
#include <cstdint>
namespace shm { struct DlAclApi {
inline static int logic_status = 0, physical_status = 0;
static int RtGetLogicDevIdByUserDevId(int32_t user, int32_t* logic) { *logic=user; return logic_status; }
static int AclrtGetPhyDevIdByLogicDevId(int32_t user, int32_t* phy) { *phy=user; return physical_status; }
}; }
''')
    # Preserve production public headers and bootstrap handle layout.
    stub('transport_def.h', '''#pragma once
#include <map>
#include <cstdint>
namespace shm::transport { struct UdmaQpConfig {
    uint32_t qpNum = 1;
    bool multiLinkEnabled = false;
    std::map<uint32_t, uint32_t> netLayerPolicy;
}; }
''')
    stub('init/shmemi_init.h', '''#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "host/init/shmem_host_init.h"
#include "utils/shmemi_host_types.h"
#include "transport/topo/topo_reader.h"
#include "transport_def.h"
struct entity_member {
    aclshmem_device_host_state_t* entity_host_state{};
    aclshmemi_bootstrap_handle_t* entity_boot_handle{};
    std::unique_ptr<shm::transport::TopologyContext> topology_context;
    std::vector<std::vector<aclshmemx_net_layer_t>> topology_net_layers_by_pe;
};
class aclshmemi_init_backend {
public:
    std::mutex entity_map_mutex_;
    std::map<uint64_t, entity_member*> entity_map_;
    int query_topology(uint64_t, int, uint32_t, aclshmemx_topology_t*);
    int build_topology_state(uint64_t);
    int validate_peer_reachability(uint64_t);
    int install_topology_context_for_test(uint64_t, std::unique_ptr<shm::transport::TopologyContext>);
    std::vector<uint32_t> test_qp_layers;
    int get_qp_net_layer(uint64_t, data_op_engine_type_t, uint32_t peer, uint32_t qp, uint32_t& layer) {
        if (peer == 0 || qp >= test_qp_layers.size()) return ACLSHMEM_INVALID_VALUE;
        layer = test_qp_layers[qp];
        return 0;
    }
    int bind_aclshmem_entity(aclshmemx_init_attr_t* attr, aclshmem_device_host_state_t* state,
        aclshmemi_bootstrap_handle_t* boot, std::nullptr_t,
        const shm::transport::UdmaQpConfig&, uint32_t, uint32_t) {
        auto* e = new entity_member;
        e->entity_host_state = state;
        e->entity_boot_handle = boot;
        entity_map_[attr->instance_id] = e;
        return 0;
    }
    int release_aclshmem_entity(uint64_t id) { delete entity_map_.at(id); entity_map_.erase(id); return 0; }
};
extern aclshmem_device_host_state_t g_state;
extern aclshmem_instance_ctx* g_instance_ctx;
extern aclshmemi_init_backend* init_manager;
''')
    production = '#include "init/shmemi_init.h"\n#include "shmemi_host_common.h"\n#include <cstddef>\n'
    production += 'aclshmem_device_host_state_t g_state{};\naclshmem_instance_ctx* g_instance_ctx{};\n'
    production += 'aclshmemi_init_backend* init_manager{};\nstatic std::mutex g_aclshmem_ctx_mutex;\n'
    src = (ROOT / 'src/host/init/shmem_init.cpp').read_text()
    start = src.index('namespace {\nconstexpr uint32_t TOPOLOGY_V1_SIZE')
    end = src.index('int32_t aclshmemx_init_attr(', start)
    production += src[start:end]
    for name in ['query_topology', 'build_topology_state', 'validate_peer_reachability']:
        production += function('src/host/init/backends/shmem_init_backend.cpp',
                               'int aclshmemi_init_backend::' + name + '(')
    production += function('src/host/init/backends/shmem_init_backend.cpp',
                           'int aclshmemi_init_backend::install_topology_context_for_test(')
    production += 'static shm::transport::UdmaQpConfig g_udma_qp_config;\nstatic bool g_qp_config_frozen = false;\n'
    production += """void ConfigureQpPolicyForTest(bool multi, uint32_t count, bool frozen) {
        g_udma_qp_config = {};
        g_udma_qp_config.multiLinkEnabled = multi;
        g_udma_qp_config.qpNum = count;
        g_qp_config_frozen = frozen;
    }
    """
    for name in ['aclshmemx_set_qp_net_layer', 'aclshmemx_get_qp_net_layer']:
        production += function('src/host/init/shmem_init.cpp', 'int ' + name + '(')
    (temp / 'queries.cpp').write_text(production)
    # Extract production configuration and capability logic; mock only memory
    # reachability and QP0 lookup. This does not emulate successful device init.
    engine_fixture = '''#pragma once
#include "shmemi_host_common.h"
#include "mem/heap/hybm_def.h"
#include "host/init/shmem_host_init.h"
namespace shm {
enum DlApiExtendLibraryType { DL_EXT_LIB_DEVICE_RDMA, DL_EXT_LIB_DEVICE_SDMA, DL_EXT_LIB_DEVICE_UDMA };
struct DlApi {
    inline static int status = 0;
    static int LoadExtendLibrary(DlApiExtendLibraryType) { return status; }
};
struct RouteFixture {
    bool routed = false;
    int GetQpNetLayer(data_op_engine_type_t engine, uint32_t, uint32_t qp, uint32_t& layer) const {
        if (engine != ACLSHMEM_DATA_OP_UDMA || qp != 0 || !routed) return ACLSHMEM_INVALID_VALUE;
        layer = 0;
        return ACLSHMEM_SUCCESS;
    }
};
struct MemEntityDefault {
    hybm_options options_{};
    RouteFixture* transportManager_ = nullptr;
    bool direct = false;
    bool SdmaReaches(uint32_t) const { return direct; }
    hybm_data_op_type CanReachDataOperators(uint32_t remoteRank) const noexcept;
    int LoadExtendLibrary() noexcept;
};
'''
    engine_fixture += function('src/host/entity/mem_entity_default.cpp',
                               'hybm_data_op_type MemEntityDefault::CanReachDataOperators(')
    engine_fixture += function('src/host/entity/mem_entity_default.cpp', 'int MemEntityDefault::LoadExtendLibrary(')
    engine_fixture += '}\ninline hybm_options ConfigureEngines(aclshmemx_init_attr_t* attributes) {\n'
    backend_source = (ROOT / 'src/host/init/backends/shmem_init_backend.cpp').read_text()
    begin = backend_source.index('    hybm_options options{};', backend_source.index('::create_entity('))
    end = backend_source.index('    options.bmScope', begin)
    engine_fixture += backend_source[begin:end] + '    return options;\n}\n'
    engine_fixture += '''inline uint32_t CmoQuietMask(aclshmemx_init_attr_t* attributes) {
    struct Element { aclshmemx_init_attr_t* entity_attr; } storage{attributes};
    auto* elem = &storage;
    (void)elem;
    struct Quiet { uint32_t quiet_transport_mask; } quiet{};
    auto* quiet_state = &quiet;
'''
    begin = backend_source.index('    quiet_state->quiet_transport_mask = 0;')
    end = backend_source.index('    for (int32_t i = 0;', begin)
    engine_fixture += backend_source[begin:end] + '    return quiet.quiet_transport_mask;\n}\n'
    stub('engine_fixture.h', engine_fixture)
    identity_fixture = """#pragma once
#include "utils/under_api/dl_comm_def.h"
#include "shmemi_host_common.h"
namespace shm {
struct MemSegment {
    AscendSocType socType_ = ASCEND_950;
    uint32_t superPodId_ = 7, serverId_ = 10, logicDeviceId_ = 0;
    bool IsSdmaAccessible(uint32_t, uint32_t, uint32_t) noexcept;
};
"""
    constants = (ROOT/'src/host/mem/heap/hybm_device_mem_segment.h').read_text()
    for line in constants.splitlines():
        if line.startswith('constexpr uint32_t invalidSuperPodId ') or line.startswith('constexpr uint32_t ASC910B_CONN_RANKS '):
            identity_fixture += line + '\n'
    identity_fixture += function('src/host/mem/heap/hybm_mem_segment.cpp', 'bool MemSegment::IsSdmaAccessible(')
    identity_fixture += '}\n'
    stub('identity_fixture.h', identity_fixture)

    gtest = args.gtest.resolve() / 'googletest'
    include_dirs = [inc, ROOT/'include', ROOT/'src/host', ROOT/'src/host/bootstrap/config_store', ROOT/'src/host/transport',
                    args.json.resolve()/'single_include', gtest/'include', gtest]
    flags = ['-std=c++17', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
             '-fsanitize=address,undefined', '-pthread', *['-I'+str(p) for p in include_dirs]]
    sources = [gtest/'src/gtest-all.cc', gtest/'src/gtest_main.cc', temp/'queries.cpp', ROOT/'tests/topology/context_test.cpp',
               ROOT/'src/host/transport/topo/topo_reader.cpp',
               ROOT/'src/host/transport/topo/topology_context.cpp',
               ROOT/'tests/unittest/host/topo/udma_route_test.cpp',
               ROOT/'tests/unittest/host/topo/topo_reader_test.cpp',
               ROOT/'tests/unittest/host/topo/topology_layers_test.cpp',
               ROOT/'tests/unittest/host/init/topology_query_test.cpp',
               ROOT/'tests/topology/engine_matrix_test.cpp',
               ROOT/'tests/topology/identity_matrix_test.cpp',
               ROOT/'tests/topology/qp_contract_test.cpp',
               ROOT/'tests/topology/rfc_requirements_test.cpp',
               ROOT/'tests/unittest/host/init/topology_json_stub_test.cpp']
    records = []
    if args.output:
        args.output = args.output.resolve()
        args.output.mkdir(parents=True, exist_ok=True)
        # Record every source/header in the actual production and test trees,
        # plus the generated translation units, not just a few entry files.
        paths = [*ROOT.joinpath('include').rglob('*.h'), *ROOT.joinpath('src/host').rglob('*.h'),
                 *ROOT.joinpath('src/host').rglob('*.cpp'),
                 *ROOT.joinpath('tests/topology').glob('*.cpp'),
                 *ROOT.joinpath('tests/unittest/host').rglob('*_test.cpp'), Path(__file__)]
        (args.output/'source-sha256.json').write_text(json.dumps({str(p.relative_to(ROOT)):
            hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(paths))}, indent=2))
        (args.output/'generated-sha256.json').write_text(json.dumps({str(p.relative_to(temp)):
            hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(temp.rglob('*')) if p.is_file()}, indent=2))
    for soc in args.platforms:
        extra = ['-DACLSHMEM_UNIT_TEST=1'] + (['-DACLSHMEM_SOC_950'] if soc == '950' else [])
        binary = temp / ('topology-' + soc)
        subprocess.run([os.environ.get('CXX', 'clang++'), *flags, *extra,
                        *map(str, sources), '-o', str(binary)], check=True)
        command = [str(binary)]
        if args.output:
            command += ['--gtest_output=xml:' + str(args.output/(soc+'.xml'))]
        result = subprocess.run(command, env={**os.environ, 'SHMEM_TOPOLOGY_TEST_DIR': str(temp)})
        records.append(dict(platform=soc, returncode=result.returncode, sanitizer='address,undefined',
                            macros=extra, scope='CPU production parser/context and extracted function bodies'))
        if args.output:
            (args.output/'receipts.json').write_text(json.dumps(records, indent=2))
        result.check_returncode()
