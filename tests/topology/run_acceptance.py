#!/usr/bin/env python3
"""Generate and run local acceptance. Device tests are explicitly NOT executed.
No Python engine/topology implementation supplies public query results.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=HERE/'results/topo-acceptance-20261019')
parser.add_argument('--topo-dir', type=Path, default=HERE/'topo', help='Exactly nine user-uploaded physical topology JSON files')
parser.add_argument('--json', type=Path, default=Path('/tmp/shmem-rfc-json-v3.11.3'))
parser.add_argument('--gtest', type=Path, default=Path('/tmp/shmem-gtest-complete/googletest-1.14.0'))
parser.add_argument('--generate-only', action='store_true')
parser.add_argument('--resume', action='store_true', help='Reuse successful command receipts; rerun failed or missing commands')
parser.add_argument('--950-biaoka-ubx', dest='biaoka_ubx', action='store_true', help='Separate 950 suite: original biaoka RootInfo and synthetic UBX RootInfo')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)


def synthetic_root(topology, name):
    """Only create explicitly synthetic device descriptions around original edges.
    Plane, address, network instance, and local->device mapping are assumptions.
    Production C++ is solely responsible for matching those descriptions.
    """
    topo = json.loads(topology.read_text())
    ranks = []
    for peer in topo['peer_list']:
        ident = peer['local_id']
        layers = {}
        for edge in topo['edge_list']:
            layer = edge['net_layer']
            ports = []
            if edge['local_a'] == ident:
                ports += edge.get('local_a_ports', [])
            if edge.get('local_b') == ident:
                ports += edge.get('local_b_ports', [])
            if ports:
                layers.setdefault(layer, set()).update(ports)
        levels = []
        for layer, ports in sorted(layers.items()):
            addresses = []
            for index, port in enumerate(sorted(ports)):
                addresses.append(dict(addr_type='IPV4' if layer == 3 else 'EID',
                    addr=f'192.0.2.{ident+1}' if layer == 3 else f'{(ident+1)*4096+layer*256+index:032x}',
                    ports=[port], plane_id=f'synthetic-plane-{layer}'))
            levels.append(dict(net_layer=layer, net_instance_id=f'synthetic-{name}-fabric-{layer}',
                net_type='CLOS' if layer == 3 else 'TOPO_FILE_DESC', net_attr='', rank_addr_list=addresses))
        ranks.append(dict(device_id=ident, local_id=ident, level_list=levels))
    path = out / (name+'-synthetic-rootinfo.json')
    path.write_text(json.dumps(dict(version='2.0', rank_count=len(ranks), rank_list=ranks,
                                   topo_file_path=str(topology.resolve())), indent=2))
    return path, len(ranks)


topo_dir = args.topo_dir.resolve()
inputs = sorted(topo_dir.glob('*.json'))
expected_names = {f'atlas_{product}_{index}.json' for product,indices in [(350,range(1,4)),(850,range(1,5)),(950,range(1,3))] for index in indices}
if not args.biaoka_ubx and (len(inputs) != 9 or {f.name for f in inputs} != expected_names):
    parser.error('--topo-dir must contain the nine atlas_350/850/950 JSON inputs')
bases = []
all_suites = {}
manifest = dict(scope='CPU-only production function bodies; no full device init',
    device_acceptance='NOT_RUN', input_source_directory=str(topo_dir),
    rootinfo_policy='Synthetic addresses/plane/fabric; original physical edges preserved',
    primary_soc='Ascend950; A2/A3 runs are public-contract branch regressions, not product identity claims', inputs=[])
if args.biaoka_ubx:
    inputs = [HERE/'cpp_json_stub/atlas_350_3.json', HERE/'cpp_json_stub/root_info-ubx.json']
    manifest.update(input_source_directory=str(HERE/'cpp_json_stub'), rootinfo_policy='Original biaoka RootInfo; UBX synthetic RootInfo', suite='950-biaoka-ubx')
for topology in inputs:
    physical = json.loads(topology.read_text())
    ids = [p['local_id'] for p in physical['peer_list']]
    assert len(set(ids)) == len(ids) == physical['peer_count'], topology
    assert len(physical['edge_list']) == physical['edge_count'], topology
    for edge in physical['edge_list']:
        assert edge['local_a'] in ids and ('local_b' not in edge or edge['local_b'] in ids), topology
    root,n = synthetic_root(topology, topology.stem)
    kind='synthetic_rootinfo'
    name=topology.stem
    if args.biaoka_ubx:
        name='biaoka' if topology.name=='atlas_350_3.json' else 'UBX'
        if name=='biaoka':
            root=HERE/'cpp_json_stub/root_info-biaoka.json'
            kind='original_rootinfo'
    bases.append((name,root,n,kind))
    manifest['inputs'].append(dict(name=name,input_kind=kind,path=str(topology),
        sha256=hashlib.sha256(topology.read_bytes()).hexdigest(),peers=n,edges=len(physical['edge_list']),
        synthetic_rootinfo=str(root),synthetic_rootinfo_sha256=hashlib.sha256(root.read_bytes()).hexdigest(),
        scope='uploaded physical edges + explicit synthetic RootInfo; no device validation'))
cleanup = Path('/tmp/shmem-topo-retest-cleanup-20261019.json')
if cleanup.exists():
    manifest['previous_output_cleanup'] = json.loads(cleanup.read_text())
for platform in (('950',) if args.biaoka_ubx else ('A2', 'A3', '950')):
    cases = []
    for topology, root, count, kind in bases:
        for config in range(1, 16):
            scenarios = ('D1_U1', 'D1_U0', 'D1_no_manager', 'D0') if platform == '950' else ('D1', 'D0')
            for relation in scenarios:
                direct = [relation != 'D0'] * count
                direct[0] = True
                routed = relation not in ('D1_U0', 'D1_no_manager')
                routes = [[]] + ([[0]] * (count-1) if routed else [[] for _ in range(count-1)])
                expected_init = -100 if ((platform != '950' and config & 8) or
                    (relation == 'D0' and not config & 4)) else 0
                engines = []
                for pe in range(count):
                    expected = (1 if direct[pe] else 0) | (config & 4)
                    if platform != '950' and direct[pe]: expected |= config & 2
                    if platform == '950' and pe and direct[pe] and routed and config & 8:
                        expected |= 8
                    engines.append(expected)
                cases.append(dict(name=f'{topology}_{platform}_config{config:02x}_{relation}',
                    topology_label=topology, platform=platform, input_kind=kind, acceptance_mode=True,
                    root_info=str(root.resolve()), engine_config=config, my_pe=0,
                    direct_reachable=direct, udma_qp_layers=routes,
                    transport_manager=relation != 'D1_no_manager', expected_init_status=expected_init,
                    expected_engine_masks=engines))
        # Each source PE for all required platform configurations.
        basic = (1, 5, 9, 13) if platform == '950' else (1, 5, 3, 7)
        for source in range(count):
            for config in basic:
                routes = [[0] if pe != source else [] for pe in range(count)]
                engines = [(1 | (config & 4) | ((config & 8) if platform == '950' and pe != source else 0)
                            | ((config & 2) if platform != '950' else 0)) for pe in range(count)]
                cases.append(dict(name=f'{topology}_{platform}_basic{config:02x}_source{source}',
                    topology_label=topology, platform=platform, input_kind=kind, acceptance_mode=True,
                    root_info=str(root.resolve()), engine_config=config, my_pe=source,
                    direct_reachable=[True]*count, udma_qp_layers=routes,
                    expected_engine_masks=engines))
        # Failures are from extracted extension-loading and production reachability gate.
        for label,extra in [('resource_failure',dict(resource_status=-9,expected_init_status=-9)),
                ('remote_unreachable',dict(remote_init_status=-100,expected_init_status=-100)),
                ('gate_bootstrap_failure',dict(gate_transport_failure=True,expected_init_status=-6))]:
            cases.append(dict(name=f'{topology}_{platform}_{label}', topology_label=topology,
                platform=platform,input_kind=kind,acceptance_mode=True, root_info=str(root.resolve()),
                engine_config=5,my_pe=0,direct_reachable=[True]*count,**extra))
        if platform == '950':
            for count_qp in (1,2,4):
                for policy_name in ('default','layer0','layer1','mixed'):
                    for multi in (False,True):
                        policy=[] if policy_name=='default' else [[q,(q%2 if policy_name=='mixed' else int(policy_name[-1]))] for q in range(count_qp)]
                        cases.append(dict(name=f'{topology}_950_qp{count_qp}_{policy_name}_multi{int(multi)}',
                            topology_label=topology,platform=platform,input_kind=kind,acceptance_mode=True,
                            root_info=str(root.resolve()),my_pe=0,engine_config=13,
                            direct_reachable=[True]*count, route_qp_count=count_qp,
                            qp_policy=policy,multi_link=multi))
    all_suites[platform] = cases
    (out/(platform+'-cases.json')).write_text(json.dumps(dict(cases=cases),indent=2))
manifest['cases']={platform:len(cases) for platform,cases in all_suites.items()}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(manifest['cases']))
if args.generate_only:
    raise SystemExit(0)
commands = [('host-unit', [sys.executable,str(HERE/'run_host_tests.py'),'--json',str(args.json),'--gtest',str(args.gtest)])]
commands += [(platform,[sys.executable,'-B',str(HERE/'cpp_json_stub/run.py'),'--platform',platform,
    '--json',str(args.json),'--fixtures',str(out/(platform+'-cases.json'))]) for platform in all_suites]
commands += [('legacy-cpp',[sys.executable,'-B',str(HERE/'cpp_json_stub/run.py'),'--json',str(args.json),'--fixtures',str(HERE/'cpp_json_stub/cases.json')])]
commands += [('python-wrapper',[sys.executable,str(HERE/'test_python_topology.py')]),
    ('python-json',[sys.executable,'-B','-m','unittest','discover','-s',str(HERE/'json_stub'),'-p','test_local_stub.py'])]
test_sources = [ROOT/'src/host/init/backends/shmem_init_backend.cpp', ROOT/'src/host/init/shmem_init.cpp',
    ROOT/'src/host/entity/mem_entity_default.cpp', ROOT/'src/host/transport/topo/topo_reader.cpp',
    ROOT/'src/host/transport/topo/topology_context.cpp',HERE/'cpp_json_stub/main.cpp',HERE/'cpp_json_stub/run.py',
    HERE/'run_host_tests.py',HERE/'identity_matrix_test.cpp',HERE/'qp_contract_test.cpp',Path(__file__)]
manifest['source_sha256'] = {str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in test_sources}
manifest['input_fingerprint'] = hashlib.sha256(json.dumps(manifest['inputs'],sort_keys=True).encode()).hexdigest()
manifest['source_fingerprint'] = hashlib.sha256(json.dumps(manifest['source_sha256'],sort_keys=True).encode()).hexdigest()
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
receipts=[]
previous = {}
if args.resume and (out/'run-receipts.json').exists():
    previous = {r['name']:r for r in json.loads((out/'run-receipts.json').read_text())}
for name,command in commands:
    prior=previous.get(name,{})
    if (prior.get('returncode') == 0 and prior.get('command') == command and
        prior.get('input_fingerprint') == manifest['input_fingerprint'] and
        prior.get('source_fingerprint') == manifest['source_fingerprint']):
        receipts.append(prior)
        print(name, 'PASS (identical input/source fingerprints)', flush=True)
        continue
    with (out/(name+'.log')).open('w') as log:
        result=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    receipts.append(dict(name=name,command=command,returncode=result.returncode,log=name+'.log',
        input_fingerprint=manifest['input_fingerprint'],source_fingerprint=manifest['source_fingerprint']))
    (out/'run-receipts.json').write_text(json.dumps(receipts,indent=2))
    print(name, 'PASS' if result.returncode==0 else 'FAIL',flush=True)
(out/'run-receipts.json').write_text(json.dumps(receipts,indent=2))
if any(r['returncode'] for r in receipts):
    raise SystemExit(1)
