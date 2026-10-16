"""Run on macOS/Linux with Python standard library; no CANN or NPU required."""
import copy
from pathlib import Path
import tempfile
import unittest

from local_stub import ENGINES, LINKS, FixtureError, JsonTopologyStub, read_json

HERE = Path(__file__).resolve().parent


class LocalTopologyStubTest(unittest.TestCase):
    def test_all_json_cases(self):
        for case in read_json(HERE / 'cases.json')['cases']:
            with self.subTest(case=case['name']):
                if not case['parse_ok']:
                    with self.assertRaises((FixtureError, KeyError)):
                        JsonTopologyStub(case, HERE)
                    continue
                stub = JsonTopologyStub(case, HERE)
                for pe in range(stub.npes):
                    for mask in (ENGINES, LINKS, ENGINES | LINKS):
                        result = stub.query_topology(pe, mask)
                        layers = case['expected_layers'][pe] if mask & LINKS else []
                        self.assertEqual(result, dict(
                            engine_mask=case['expected_engine_masks'][pe] if mask & ENGINES else 0,
                            net_layer_count=len(layers), net_layers=layers))

    def test_query_validation_and_finalize(self):
        case = read_json(HERE / 'cases.json')['cases'][0]
        stub = JsonTopologyStub(case, HERE)
        for peer in (-1, stub.npes, True):
            with self.assertRaises(ValueError):
                stub.query_topology(peer)
        for mask in (0, 4, -1, True):
            with self.assertRaises(ValueError):
                stub.query_topology(1, mask)
        result = stub.query_topology(1)
        result['net_layers'].clear()
        self.assertEqual(stub.query_topology(1)['net_layers'], [0, 1, 3])
        stub.finalize()
        self.assertEqual(result['engine_mask'], 15)
        with self.assertRaises(RuntimeError):
            stub.query_topology(1)

    def test_reverse_source_and_independent_snapshots(self):
        case = read_json(HERE / 'cases.json')['cases'][0]
        first = JsonTopologyStub(case, HERE)
        other = copy.deepcopy(case)
        other['my_pe'] = 1
        other['internal_transport_masks'] = [1, 1, 1, 1]
        second = JsonTopologyStub(other, HERE)
        self.assertEqual(second.query_topology(0)['net_layers'], [0, 1, 3])
        self.assertEqual(second.query_topology(1)['net_layers'], [])
        self.assertEqual(first.query_topology(1)['engine_mask'], 15)
        self.assertEqual(second.query_topology(0)['engine_mask'], 1)
        first.finalize()
        self.assertEqual(second.query_topology(0)['engine_mask'], 1)

    def test_merged_layer_and_topology_instance_boundary(self):
        case = copy.deepcopy(read_json(HERE / 'cases.json')['cases'][0])
        root = read_json(HERE / 'rootinfo.json')
        topo = read_json(HERE / 'topo.json')
        for rank in root['rank_list']:
            for level in rank['level_list'][:2]:
                level['net_type'] = 'TOPO_FILE_DESC'
                level['net_layer'] = 0
        for edge in topo['edge_list']:
            if edge.get('topo_type') == 'CLOS':
                edge['net_layer'] = 0
        with tempfile.TemporaryDirectory() as temp:
            import json
            directory = Path(temp)
            (directory / 'rootinfo.json').write_text(json.dumps(root))
            (directory / 'topo.json').write_text(json.dumps(topo))
            self.assertEqual(JsonTopologyStub(case, directory).query_topology(1)['net_layers'], [0, 1, 3])
            next(e for e in topo['edge_list'] if e.get('topo_type') == 'CLOS' and e['local_a'] == 1)['topo_instance_id'] = 99
            (directory / 'topo.json').write_text(json.dumps(topo))
            self.assertEqual(JsonTopologyStub(case, directory).query_topology(1)['net_layers'], [0, 3])

    def test_missing_and_malformed_files(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FixtureError):
                read_json(Path(temp) / 'missing.json')
            path = Path(temp) / 'invalid.json'
            path.write_text('{')
            with self.assertRaises(FixtureError):
                read_json(path)


if __name__ == '__main__':
    unittest.main()
