# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Source integration audits, explicitly NOT CANN builds or device tests."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]

def source(path):
    return (ROOT/path).read_text()

class RfcIntegrationAudit(unittest.TestCase):
    def test_init_builds_snapshot_and_gate_before_publishing_initialized(self):
        s = source('src/host/init/shmem_init.cpp')
        begin = s.index('static int32_t aclshmemi_init_attr_impl(')
        s = s[begin:s.index('namespace {\nconstexpr uint32_t TOPOLOGY_V1_SIZE', begin)]
        ordered = ['init_manager->bind_aclshmem_entity(', 'init_manager->reserve_heap()',
                   'init_manager->setup_heap()', 'init_manager->validate_peer_reachability(',
                   'memory_manager_initialize(', 'init_manager->build_topology_state(',
                   'g_state.is_aclshmem_initialized = true;']
        positions = [s.index(x) for x in ordered]
        self.assertEqual(positions, sorted(positions))

    def test_create_entity_discovers_once_for_device_and_passes_borrowed_context(self):
        s = source('src/host/init/backends/shmem_init_backend.cpp')
        start = s.index('int aclshmemi_init_backend::create_entity(')
        s = s[start:s.index('int aclshmemi_init_backend::update_device_state(', start)]
        self.assertRegex(s, r'#ifdef ACLSHMEM_SOC_950\s+if \(mem_type == DEVICE_SIDE\)')
        self.assertEqual(s.count('topology_context->Initialize('), 1)
        self.assertIn('transport_options.topologyContext = elem->topology_context.get();', s)
        self.assertLess(s.index('topology_context->Initialize('), s.index('hybm_create_entity_with_transport_options('))
        self.assertIn('const TopologyContext* topologyContext{nullptr};', source('src/host/transport/transport_def.h'))

    def test_public_include_export_and_python_native_copy_are_wired(self):
        self.assertIn('#include "host/init/shmem_host_init.h"', source('include/shmem.h'))
        self.assertIn('ACLSHMEM_HOST_API int aclshmemx_query_topology(', source('include/host/init/shmem_host_init.h'))
        s = source('src/host/python_wrapper/pyshmem.cpp')
        start = s.index('"aclshmemx_query_topology",')
        s = s[start:s.index('    m.def(', start)]
        self.assertIn('ACLSHMEMX_TOPOLOGY_INITIALIZER', s)
        self.assertIn('py::make_tuple(status, py::none())', s)
        self.assertIn('layers.append(static_cast<uint32_t>(topology.net_layers[i]))', s)
        self.assertNotIn('gil_scoped_release', s)
        # This is only source wiring; real pybind compilation/linking remains NOT_RUN.

    def test_cmake_source_globs_and_non950_exclusion_rules_are_present(self):
        s = source('src/CMakeLists.txt')
        self.assertRegex(s, r'GLOB_RECURSE ACLSHMEM_HOST_FILES[^\n]*host/\*\.cpp')
        self.assertRegex(s, r'if\(NOT SOC_TYPE STREQUAL "Ascend950"\)\s+'
                            r'list\(FILTER ACLSHMEM_HOST_FILES EXCLUDE REGEX "host/transport/topo"\)')
        self.assertIn('GLOB_RECURSE TOPO_UNITTEST_SOURCE_FILES', source('tests/unittest/host/CMakeLists.txt'))
        self.assertTrue((ROOT/'src/host/transport/topo/topology_context.cpp').is_file())
        # Does not execute CMake or prove platform linkability.

    def test_example_verifies_actual_resources_before_launch(self):
        s = source('examples/udma_qp_demo/main.cpp')
        start = s.index('int main('); s = s[start:]
        self.assertLess(s.index('VerifyTwoLayerPlan('), s.index('launch_udma_qp_demo('))
        self.assertIn('ACLSHMEM_TOPOLOGY_DEMO', s)
        build = source('examples/topology_query/CMakeLists.txt')
        self.assertIn('ACLSHMEM_TOPOLOGY_DEMO=1', build)
        self.assertIn('udma_qp_demo_kernel', build)

    def test_query_reads_cache_without_rebuilding_or_collective(self):
        s = source('src/host/init/backends/shmem_init_backend.cpp')
        start = s.index('int aclshmemi_init_backend::query_topology(')
        s = s[start:s.index('#if defined(ACLSHMEM_UNIT_TEST)', start)]
        for forbidden in ['allgather(', 'Initialize(', 'GetReachableNetLayers(', 'build_topology_state(']:
            self.assertNotIn(forbidden, s)
        self.assertIn('topology_net_layers_by_pe[pe]', s)
        self.assertIn('layers.data()', s)

if __name__ == '__main__':
    unittest.main()
