#!/usr/bin/env python3
"""Validate fixture files/schema only; does not simulate or verify C++ topology matching."""
import argparse
import ipaddress
import json
from pathlib import Path


def check(path):
    suite = json.loads(path.read_text())
    assert isinstance(suite['cases'], list) and suite['cases'], 'cases must be nonempty'
    names = set()
    for case in suite['cases']:
        name = case['name']
        assert name not in names, f'duplicate case {name}'
        names.add(name)
        count = len(case['devices'])
        assert count and len(set(case['devices'])) == count
        assert 0 <= case['my_pe'] < count
        assert isinstance(case['parse_ok'], bool)
        for key in ('internal_transport_masks', 'expected_engine_masks', 'expected_layers'):
            assert len(case[key]) == count, (name, key)
        for internal, public in zip(case['internal_transport_masks'], case['expected_engine_masks']):
            assert type(internal) is int and 0 <= internal <= 15
            # Published transport-to-engine encoding, independent of physical LINKS.
            assert public == ((internal & 9) | ((internal & 2) << 1) | ((internal & 4) >> 1)), name
        for layers in case['expected_layers']:
            assert layers == sorted(set(layers)) and set(layers) <= {0, 1, 3}, name
        assert case['expected_layers'][case['my_pe']] == [], name
        root = json.loads((path.parent / case['root_info']).read_text())
        topo = json.loads((path.parent / case['topology']).read_text())
        if not case['parse_ok']:
            continue  # Deliberately invalid business input, checked by C++ negative cases.
        assert isinstance(root['topo_file_path'], str)
        assert topo['edge_list'], name
        devices = {r['device_id']: r for r in root['rank_list']}
        for device in case['devices']:
            rank = devices[device]
            assert type(rank['local_id']) is int
            for level in rank['level_list']:
                assert level['net_type'] in ('MESH', 'CLOS', 'TOPO_FILE_DESC')
                assert isinstance(level['net_instance_id'], str)
                for addr in level['rank_addr_list']:
                    assert isinstance(addr['plane_id'], str)
                    assert isinstance(addr['ports'], list)
                    if addr['addr_type'] in ('IPV4', 'IPV6'):
                        parsed = ipaddress.ip_address(addr['addr'])
                        assert parsed.version == (4 if addr['addr_type'] == 'IPV4' else 6)
        print(f'OK schema: {name}')
    print(f'{len(names)} fixture cases validated (schema only; run C++ UT for query results).')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cases', nargs='?', type=Path, default=Path(__file__).with_name('cases.json'))
    args = parser.parse_args()
    check(args.cases.resolve())
