#!/usr/bin/env python3
"""TEMP_TOPOLOGY_JSON_STUB: CPU-only model, not the native SHMEM library."""
import argparse
import ipaddress
import json
from pathlib import Path

ENGINES = 1
LINKS = 2


class FixtureError(ValueError):
    """Invalid or unusable JSON topology input."""


def read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as error:
        raise FixtureError(f'{path}: {error}') from error


def uint(value):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise FixtureError(f'Expected uint32, got {value!r}')
    return value


def ports(value):
    if not isinstance(value, list) or any(not isinstance(p, str) for p in value):
        raise FixtureError('ports must be a string array')
    return tuple(value)


def usable(domain):
    return bool(domain) and domain.lower() != 'unknown'


class JsonTopologyStub:
    """Instance-owned snapshot. ENGINES is injected; LINKS is computed.

    Mirrors GetReachableNetLayers/PhysicalEndpointPairMatches/HasRoceLayerRelation
    in topo_reader.cpp and the engine conversion in backend::query_topology.
    No runtime, bootstrap, QP, memory mapping, C++ ABI or health checks are run.
    """

    def __init__(self, case, directory):
        self.initialized = False
        self.my_pe = uint(case['my_pe'])
        devices = [uint(d) for d in case['devices']]
        self.npes = len(devices)
        if not self.npes or self.my_pe >= self.npes or len(set(devices)) != self.npes:
            raise FixtureError('Invalid devices or my_pe')
        self.masks = tuple(uint(m) for m in case['internal_transport_masks'])
        if len(self.masks) != self.npes or any(m > 15 for m in self.masks):
            raise FixtureError('Expected one internal transport mask (0..15) per PE')
        directory = Path(directory)
        root = read_json(directory / case['root_info'])
        if not isinstance(root.get('topo_file_path'), str):
            raise FixtureError('Missing topo_file_path')
        topo = read_json(directory / case['topology'])
        self.mesh, self.clos = [], []
        for edge in topo['edge_list']:
            if 'local_a' not in edge:
                continue
            a = uint(edge['local_a'])
            if edge.get('topo_type') == 'CLOS' or 'local_b' not in edge:
                self.clos.append((a, uint(edge['net_layer']), uint(edge['topo_instance_id']),
                                  ports(edge.get('local_a_ports', []))))
            else:
                self.mesh.append((a, uint(edge['local_b']), ports(edge.get('local_a_ports', [])),
                                  ports(edge.get('local_b_ports', []))))
        if not self.mesh and not self.clos:
            raise FixtureError('No valid topology edges')
        self.clos.sort(key=lambda e: e[1])
        self.ids, self.endpoints, all_levels = [], [], []
        for device in devices:
            ranks = [r for r in root['rank_list'] if uint(r['device_id']) == device]
            if not ranks:
                raise FixtureError(f'No rank entry for device {device}')
            rank = ranks[0]
            self.ids.append(uint(rank['local_id']))
            levels = rank['level_list']
            endpoints = []
            for level in levels:
                layer = uint(level['net_layer'])
                kind, domain = level['net_type'], level['net_instance_id']
                if kind not in ('MESH', 'CLOS', 'TOPO_FILE_DESC') or not isinstance(domain, str):
                    raise FixtureError('Invalid network type/instance')
                addresses = level['rank_addr_list']
                if not isinstance(addresses, list) or not addresses:
                    raise FixtureError('Empty rank_addr_list')
                for address in addresses:
                    # Production parser skips malformed individual addresses.
                    try:
                        family = address.get('addr_type', 'EID').upper()
                        value = address['addr']
                        if family in ('IPV4', 'IPV6'):
                            ip = ipaddress.ip_address(value)
                            if ip.version != (4 if family == 'IPV4' else 6):
                                continue
                        elif family == 'EID':
                            if isinstance(value, list):
                                if len(value) != 16 or any(type(v) is not int or not 0 <= v <= 255 for v in value):
                                    continue
                            else:
                                text = value[2:] if value.startswith(('0x', '0X')) else value
                                if len(text) != 32 or any(c not in '0123456789abcdefABCDEF' for c in text):
                                    continue
                        else:
                            continue
                        plane = address['plane_id']
                        if not isinstance(plane, str):
                            continue
                        endpoints.append(dict(layer=layer, kind=kind, domain=domain, plane=plane,
                                              family=family, ports=ports(address.get('ports', []))))
                    except (KeyError, ValueError, TypeError, AttributeError):
                        continue
            if not endpoints:
                raise FixtureError(f'No usable addresses for device {device}')
            self.endpoints.append(endpoints)
            all_levels.append(levels)
        self.merged = (any(l['net_layer'] == 0 and l['net_type'] == 'TOPO_FILE_DESC'
                           for l in all_levels[self.my_pe])
                       and bool(self.mesh) and any(e[1] == 0 for e in self.clos))
        self.layers = tuple(tuple(self._reachable(pe)) for pe in range(self.npes))
        self.initialized = True

    def _has_port(self, endpoint, port, fabric):
        if port not in endpoint['ports']:
            return False
        if endpoint['kind'] in ('MESH', 'CLOS'):
            return endpoint['kind'] == fabric
        if fabric == 'MESH':
            return any(port in e[2] or port in e[3] for e in self.mesh)
        return any(port in e[3] for e in self.clos)

    def _pair(self, a, b, lp, pp, fabric, layer):
        if not self._has_port(a, lp, fabric) or not self._has_port(b, pp, fabric):
            return False
        same_domain = (a['layer'] == b['layer'] == layer and usable(a['domain'])
                       and a['domain'] == b['domain'])
        if fabric == 'MESH':
            return same_domain
        if not a['plane'] or a['plane'] != b['plane']:
            return False
        return same_domain if a['kind'] == 'CLOS' or b['kind'] == 'CLOS' else True

    def _reachable(self, peer):
        if peer == self.my_pe:
            return []
        local, remote = self.endpoints[self.my_pe], self.endpoints[peer]

        def has_pair(lports, pports, fabric, layer):
            return any(self._pair(a, b, lp, pp, fabric, layer)
                       for lp in lports for pp in pports for a in local for b in remote)

        layers = set()
        source_id, peer_id = self.ids[self.my_pe], self.ids[peer]
        for a, b, ap, bp in self.mesh:
            if a == source_id and b == peer_id and has_pair(ap, bp, 'MESH', 0):
                layers.add(0)
            elif b == source_id and a == peer_id and has_pair(bp, ap, 'MESH', 0):
                layers.add(0)
        for a, layer, instance, ap in self.clos:
            if a != source_id:
                continue
            for b, other_layer, other_instance, bp in self.clos:
                if (b == peer_id and layer == other_layer and instance == other_instance
                        and has_pair(ap, bp, 'CLOS', layer)):
                    effective = 1 if self.merged and layer == 0 else layer
                    if effective in (0, 1):
                        layers.add(effective)
        if any(a['layer'] == b['layer'] == 3 and a['kind'] == b['kind'] == 'CLOS'
               and usable(a['domain']) and a['domain'] == b['domain']
               and a['plane'] and a['plane'] == b['plane']
               and a['family'] == b['family'] and a['family'] in ('IPV4', 'IPV6')
               for a in local for b in remote):
            layers.add(3)
        return sorted(layers)

    def query_topology(self, pe, query_mask=ENGINES | LINKS):
        if not self.initialized:
            raise RuntimeError('ACLSHMEM_NOT_INITED (local stub)')
        if type(pe) is not int or not 0 <= pe < self.npes:
            raise ValueError('Invalid target PE')
        if type(query_mask) is not int or query_mask == 0 or query_mask & ~3:
            raise ValueError('Invalid query mask')
        internal = self.masks[pe]
        engine = (internal & 9) | ((internal & 2) << 1) | ((internal & 4) >> 1)
        layers = list(self.layers[pe]) if query_mask & LINKS else []
        return dict(engine_mask=engine if query_mask & ENGINES else 0,
                    net_layer_count=len(layers), net_layers=layers)

    def finalize(self):
        self.initialized = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path, default=Path(__file__).with_name('cases.json'))
    parser.add_argument('--case', default='mixed_engines_and_layers')
    parser.add_argument('--pe', type=int, help='Target PE; omitted means all peers')
    parser.add_argument('--mask', type=lambda text: int(text, 0), default=3)
    args = parser.parse_args()
    try:
        suite = read_json(args.fixtures)
        case = next((c for c in suite['cases'] if c['name'] == args.case), None)
        if case is None:
            raise FixtureError(f'Unknown case {args.case!r}')
        snapshot = JsonTopologyStub(case, args.fixtures.resolve().parent)
        peers = [args.pe] if args.pe is not None else range(snapshot.npes)
        for peer in peers:
            print(json.dumps(dict(mode='local_python_stub', case=case['name'],
                                  source_pe=snapshot.my_pe, target_pe=peer,
                                  **snapshot.query_topology(peer, args.mask))))
    except (FixtureError, KeyError, TypeError, ValueError, RuntimeError) as error:
        parser.exit(1, f'local stub error: {error}\n')


if __name__ == '__main__':
    main()
