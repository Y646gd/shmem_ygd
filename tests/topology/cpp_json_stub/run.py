#!/usr/bin/env python3
"""TEMP_TOPOLOGY_CPP_JSON_STUB: compile real C++ topology queries on macOS/Linux.
Python orchestrates compilation only. No Python topology implementation is used.
"""
import argparse
import json
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json', type=Path, help='nlohmann/json checkout (single_include/nlohmann/json.hpp)')
parser.add_argument('--fixtures', type=Path, help='Expected-value test suite; overrides direct RootInfo mode')
parser.add_argument('--root-info', type=Path, default=HERE/'root_info-biaoka.json')
parser.add_argument('--topology', type=Path, help='Override the topology file referenced by RootInfo')
parser.add_argument('--source-pe', type=int, default=0)
parser.add_argument('--transport-mask', type=lambda value: int(value, 0), default=1,
                    help='Mock INTERNAL transport bits: MTE=1 ROCE=2 SDMA=4 UDMA=8')
parser.add_argument('--case', help='Run just one named JSON case')
args = parser.parse_args()
if args.json is None:
    candidates = [ROOT / '3rdparty/json', Path('/private/tmp/shmem-rfc-json-v3.11.3')]
    args.json = next((p for p in candidates if (p/'single_include/nlohmann/json.hpp').is_file()), None)
if args.json is None or not (args.json/'single_include/nlohmann/json.hpp').is_file():
    parser.error('Supply --json /path/to/nlohmann-json; no CANN or GoogleTest required')

def function(path, name):
    source = (ROOT / path).read_text()
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return '#line ' + str(source[:start].count('\n') + 1) + ' \"' + str(ROOT / path) + '\"\n' + source[start:end] + '\n'


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
static bool Realpath(std::string& p) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(p, error);
    if (error) return false;
    p = resolved.string();
    return true;
}
static bool IsFile(const std::string& p) { return std::filesystem::is_regular_file(p); }
static bool CheckFileSize(const std::string& p) { return std::filesystem::file_size(p) < 10*1024*1024; }
}; }
''')
    stub('dl_acl_api.h', '''#pragma once
#include <cstdint>
namespace shm { struct DlAclApi {
static int RtGetLogicDevIdByUserDevId(int32_t user, int32_t* logic) { *logic=user; return 0; }
static int AclrtGetPhyDevIdByLogicDevId(int32_t user, int32_t* phy) { *phy=user; return 0; }
}; }
''')
    # Preserve production public headers and bootstrap handle layout.
    stub('transport_def.h', '''#pragma once
namespace shm::transport { struct UdmaQpConfig {}; }
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
    production = '#include "init/shmemi_init.h"\n#include <cstddef>\n'
    production += 'aclshmem_device_host_state_t g_state{};\naclshmem_instance_ctx* g_instance_ctx{};\n'
    production += 'aclshmemi_init_backend* init_manager{};\nstatic std::mutex g_aclshmem_ctx_mutex;\n'
    src = (ROOT / 'src/host/init/shmem_init.cpp').read_text()
    start = src.index('namespace {\nconstexpr uint32_t TOPOLOGY_V1_SIZE')
    end = src.index('int32_t aclshmemx_init_attr(', start)
    production += '#line ' + str(src[:start].count('\n') + 1) + ' \"' + str(ROOT / 'src/host/init/shmem_init.cpp') + '\"\n' + src[start:end]
    for name in ['query_topology', 'build_topology_state']:
        production += function('src/host/init/backends/shmem_init_backend.cpp',
                               'int aclshmemi_init_backend::' + name + '(')
    (temp / 'queries.cpp').write_text(production)
    include_dirs = [inc, ROOT/'include', ROOT/'src/host',
                    ROOT/'src/host/bootstrap/config_store', ROOT/'src/host/transport',
                    args.json.resolve()/'single_include']
    flags = ['-std=c++17', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-missing-field-initializers', '-fsanitize=address,undefined', '-pthread',
             '-DACLSHMEM_SOC_950',
             '-DACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO="' + str(temp/'rootinfo.json') + '"',
             *['-I'+str(p) for p in include_dirs]]
    sources = [temp/'queries.cpp', HERE/'main.cpp',
               ROOT/'src/host/transport/topo/topo_reader.cpp',
               ROOT/'src/host/transport/topo/topology_context.cpp']
    binary = temp/'cpp-topology-json-stub'
    print('Compiling production C++ parser/context/query/build with ASan+UBSan...', flush=True)
    subprocess.run([os.environ.get('CXX', 'clang++'), *flags, *map(str, sources), '-o', str(binary)], check=True)
    if args.fixtures is not None or args.case:
        fixture_path = args.fixtures.resolve() if args.fixtures else HERE/'cases.json'
    else:
        spec = dict(name=args.root_info.stem, root_info=str(args.root_info.resolve()),
                    my_pe=args.source_pe, internal_transport_mask=args.transport_mask)
        if args.topology is not None:
            spec['topology'] = str(args.topology.resolve())
        fixture_path = temp/'input-case.json'
        fixture_path.write_text(json.dumps(dict(cases=[spec])))
    command = [str(binary), str(fixture_path)]
    if args.case:
        command.append(args.case)
    subprocess.run(command, check=True)
