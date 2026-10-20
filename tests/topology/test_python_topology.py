# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Wrapper tests using an isolated native binding fixture; no torch/CANN required."""
import ast
import importlib.util
from pathlib import Path
import sys
import threading
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class TopologyPythonTest(unittest.TestCase):
    def setUp(self):
        self.native = types.ModuleType('shmem._pyshmem')
        self.utils = types.ModuleType('shmem.core.utils')
        self.utils.AclshmemError = type('AclshmemError', (Exception,), {})
        self.utils.AclshmemInvalid = type('AclshmemInvalid', (Exception,), {})
        self.utils._instance_lock = threading.RLock()
        modules = {'shmem': types.ModuleType('shmem'), 'shmem.core': types.ModuleType('shmem.core'),
                   'shmem._pyshmem': self.native, 'shmem.core.utils': self.utils}
        self.patch = patch.dict(sys.modules, modules)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        spec = importlib.util.spec_from_file_location('topology_under_test', ROOT/'src/python/shmem/core/topology.py')
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.calls = []
        self.layers = [0, 1, 3]
        def query(pe, mask):
            self.assertTrue(self.utils._instance_lock._is_owned())
            self.calls.append((pe, mask))
            if not mask:
                return -7, None
            payload = {}
            if mask & 1: payload['engine_mask'] = 9
            if mask & 2: payload['net_layers'] = list(self.layers)
            return 0, payload
        self.native.aclshmemx_query_topology = query

    def test_requested_keys_masks_and_copy(self):
        self.assertEqual(self.module.query_topology(1), {'engine_mask': 9})
        result = self.module.query_topology(1, engines=False, links=True)
        self.assertEqual(result, {'net_layers': [0, 1, 3]})
        self.assertEqual(self.module.query_topology(1, links=True), {'engine_mask': 9, 'net_layers': [0, 1, 3]})
        self.assertEqual(self.calls, [(1, 1), (1, 2), (1, 3)])
        self.layers.clear()
        self.assertEqual(result['net_layers'], [0, 1, 3])

    def test_status_is_not_silently_downgraded(self):
        with self.assertRaisesRegex(self.utils.AclshmemError, 'ret=-7'):
            self.module.query_topology(1, engines=False)
        self.native.aclshmemx_query_topology = lambda *_: (-9, None)
        with self.assertRaisesRegex(self.utils.AclshmemError, 'ret=-9'):
            self.module.query_topology(1, links=True)

    def test_input_validation(self):
        for value in [True, 1.5, '1', 1 << 31]:
            with self.assertRaises(self.utils.AclshmemInvalid): self.module.query_topology(value)
        with self.assertRaises(self.utils.AclshmemInvalid): self.module.query_topology(1, engines=1)
        with self.assertRaises(self.utils.AclshmemInvalid): self.module.query_topology(1, links='yes')
        self.assertEqual(self.calls, [])

    def test_cpp_integer_boundaries_are_passed_without_python_coercion(self):
        for value in [-(1 << 31), -1, 0, (1 << 31) - 1]:
            self.module.query_topology(value)
            self.assertEqual(self.calls[-1], (value, 1))
        for value in [-(1 << 31) - 1, 1 << 31, False, None]:
            with self.assertRaises(self.utils.AclshmemInvalid):
                self.module.query_topology(value)

    def test_native_statuses_become_errors_including_unsupported_links(self):
        for status in [-1, -2, -4, -5, -6, -8, -100]:
            self.native.aclshmemx_query_topology = lambda *_, value=status: (value, None)
            with self.subTest(status=status):
                with self.assertRaisesRegex(self.utils.AclshmemError, f'ret={status}'):
                    self.module.query_topology(1, links=True)

    def test_instance_lock_blocks_other_thread_during_native_call_and_releases_on_error(self):
        acquired = []
        def native(*_):
            def competing_finalize():
                ok = self.utils._instance_lock.acquire(blocking=False)
                acquired.append(ok)
                if ok:
                    self.utils._instance_lock.release()
            thread = threading.Thread(target=competing_finalize)
            thread.start(); thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            return -5, None
        self.native.aclshmemx_query_topology = native
        with self.assertRaises(self.utils.AclshmemError):
            self.module.query_topology(1)
        self.assertEqual(acquired, [False])
        self.assertFalse(self.utils._instance_lock._is_owned())

    def test_core_exports_are_wired(self):
        source = (ROOT/'src/python/shmem/core/__init__.py').read_text()
        ast.parse(source)
        self.assertIn('from .topology import *', source)
        self.assertIn('_topology.__all__', source)
        self.assertEqual(self.module.__all__, ['query_topology'])


if __name__ == '__main__':
    unittest.main()
