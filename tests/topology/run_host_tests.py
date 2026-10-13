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

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json', required=True, type=Path, help='nlohmann/json checkout')
parser.add_argument('--gtest', required=True, type=Path, help='googletest checkout')
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
    production += src[start:end]
    for name in ['query_topology', 'build_topology_state']:
        production += function('src/host/init/backends/shmem_init_backend.cpp',
                               'int aclshmemi_init_backend::' + name + '(')
    (temp / 'queries.cpp').write_text(production)
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
               ROOT/'tests/unittest/host/init/topology_query_test.cpp']
    for soc, extra in [('910', []), ('950', ['-DACLSHMEM_SOC_950'])]:
        binary = temp / ('topology-' + soc)
        subprocess.run([os.environ.get('CXX', 'clang++'), *flags, *extra,
                        *map(str, sources), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'SHMEM_TOPOLOGY_TEST_DIR': str(temp)})
