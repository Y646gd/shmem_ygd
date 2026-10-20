#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""RFC-wide CPU UT evidence. Hardware acceptance and actual CANN builds stay NOT_RUN."""
import argparse
import concurrent.futures
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--json', type=Path, default=Path('/tmp/shmem-rfc-json-v3.11.3'))
p.add_argument('--gtest', type=Path, default=Path('/tmp/shmem-gtest-complete/googletest-1.14.0'))
p.add_argument('--rfc', type=Path, default=ROOT.parent/'RFC.md')
p.add_argument('--output', type=Path, default=HERE/'results/rfc-ut-20261020')
p.add_argument('--resume', action='store_true', help='Reuse passing commands only if RFC and all tested source/input files match')
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
py = [sys.executable, '-B']

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def snapshot():
    paths = []
    for folder in ['include', 'src', 'examples', 'tests']:
        for f in (ROOT/folder).rglob('*'):
            if not f.is_file() or any(x in f.parts for x in ['results', '__pycache__', 'build']):
                continue
            if f.suffix in {'.cpp', '.h', '.hpp', '.py', '.json', '.cmake'} or f.name == 'CMakeLists.txt':
                paths.append(f)
    paths += [ROOT/'CMakeLists.txt', ROOT/'scripts/build.sh']
    return {str(f.relative_to(ROOT)): digest(f) for f in sorted(set(paths))}

manifest = dict(date=datetime.now(timezone(timedelta(hours=8))).isoformat(),
    root=str(ROOT), git_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
    rfc_path=str(a.rfc.resolve()), rfc_sha256=digest(a.rfc), sources=snapshot(),
    python=sys.version, host=platform.platform(), compiler=subprocess.check_output(
        [os.environ.get('CXX', 'clang++'), '--version'], text=True).splitlines()[0],
    dependencies={str(a.json/'single_include/nlohmann/json.hpp'):digest(a.json/'single_include/nlohmann/json.hpp'),
                  str(a.gtest/'googletest/src/gtest-all.cc'):digest(a.gtest/'googletest/src/gtest-all.cc')},
    scope='CPU-only; actual production topo sources and extracted control-flow bodies; explicit runtime/resource stubs')
previous = {}
if a.resume and (out/'manifest.json').is_file() and (out/'receipts.json').is_file():
    old = json.loads((out/'manifest.json').read_text())
    reporter = str(Path(__file__).relative_to(ROOT))
    # Reporter-only changes may fix report formatting/validation without recompiling
    # already-passing test binaries. Every production/test source and input must match.
    tested = lambda sources: {k:v for k,v in sources.items() if k != reporter}
    if tested(old['sources']) == tested(manifest['sources']) and old['rfc_sha256'] == manifest['rfc_sha256']:
        previous = {r['name']:r for r in json.loads((out/'receipts.json').read_text())}
(out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
receipts = []

def run(name, command):
    prior = previous.get(name, {})
    if prior.get('returncode') == 0 and prior.get('command') == command and (out/(name+'.log')).is_file():
        return {**prior, 'reused': True, 'log_sha256': digest(out/(name+'.log'))}
    start = time.monotonic()
    with (out/(name+'.log')).open('w') as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    return dict(name=name, command=command, returncode=result.returncode,
                elapsed_seconds=round(time.monotonic()-start,3), log=name+'.log', reused=False,
                log_sha256=digest(out/(name+'.log')))

def retain(records):
    receipts.extend(records)
    (out/'receipts.json').write_text(json.dumps(receipts,ensure_ascii=False,indent=2)+'\n')
    for r in records:
        print(r['name'], 'PASS' if r['returncode']==0 else 'FAIL', flush=True)
    if any(r['returncode'] for r in records):
        raise SystemExit('Check failed; inspect output logs. No compliance PASS report was issued.')

commands = [
 ('host', py+[str(HERE/'run_host_tests.py'), '--json', str(a.json), '--gtest', str(a.gtest),
              '--platforms','A2','A3','950','--output',str(out/'host')]),
 ('planner', py+[str(HERE/'run_planner_tests.py'),'--gtest',str(a.gtest),'--output',str(out/'planner')]),
 ('udma', py+[str(HERE/'run_udma_merge_tests.py'),'--json',str(a.json)]),
 ('python', py+['-m','unittest','discover','-s',str(HERE),'-p','test_python_topology.py','-v']),
 ('source-audit', py+['-m','unittest','discover','-s',str(HERE),'-p','test_rfc_integration.py','-v']),
 ('python-json', py+['-m','unittest','discover','-s',str(HERE/'json_stub'),'-p','test_local_stub.py','-v']),
]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
    futures = [executor.submit(run, *cmd) for cmd in commands]
    retain([f.result() for f in futures])

# Fresh fixture integration, not a relabeling of the previous full PE matrix run.
fixtures = out/'fixtures'; fixtures.mkdir(exist_ok=True)
retain([run('generate-nine', py+[str(HERE/'run_acceptance.py'),'--generate-only','--output',str(fixtures)])])
cases = json.loads((fixtures/'950-cases.json').read_text())['cases']
selected = []
for c in cases:
    n = len(c['direct_reachable'])
    if ('_basic' in c['name'] and c['my_pe'] in {0,n-1}) or any(x in c['name'] for x in [
            '_resource_failure','_remote_unreachable','_gate_bootstrap_failure','_config04_D0']):
        selected.append(c)
(fixtures/'950-sampled-cases.json').write_text(json.dumps(dict(cases=selected),indent=2))
retain([run('nine-fixture-samples', py+[str(HERE/'cpp_json_stub/run.py'),'--json',str(a.json),
                                     '--fixtures',str(fixtures/'950-sampled-cases.json')])])
biaoka = out/'biaoka-ubx'; biaoka.mkdir(exist_ok=True)
retain([run('generate-biaoka', py+[str(HERE/'run_acceptance.py'),'--generate-only','--950-biaoka-ubx',
                                 '--output',str(biaoka)])])
retain([run('biaoka-ubx', py+[str(HERE/'cpp_json_stub/run.py'),'--json',str(a.json),
                            '--fixtures',str(biaoka/'950-cases.json')])])
retain([run('legacy-cpp', py+[str(HERE/'cpp_json_stub/run.py'),'--json',str(a.json),
                            '--fixtures',str(HERE/'cpp_json_stub/cases.json')])])

# Freeze the evidence against both RFC and source changes during the run.
assert snapshot() == manifest['sources'], 'Sources changed during tests; rerun to issue a consistent report.'
assert digest(a.rfc) == manifest['rfc_sha256'], 'RFC changed during tests.'
gtests = []; passed_ids = set()
for f in [out/'host/A2.xml',out/'host/A3.xml',out/'host/950.xml',out/'planner/planner.xml']:
    xml = ET.parse(f).getroot()
    assert int(xml.attrib.get('failures','0')) == 0 and int(xml.attrib.get('errors','0')) == 0
    active = []
    for t in xml.iter('testcase'):
        if t.find('skipped') is not None or t.attrib.get('status') == 'notrun': continue
        assert t.find('failure') is None
        ident = t.attrib['classname']+'.'+t.attrib['name']; passed_ids.add(ident); active.append(ident)
    gtests.append(dict(xml=str(f.relative_to(out)),passed=len(active),tests=active))

requirements = json.loads((HERE/'rfc_requirements.json').read_text())
for req in requirements:
    for evidence in req['evidence']:
        if evidence in passed_ids: continue
        if '.' in evidence and evidence.split('.')[0] in ['TopologyLayersTest','RfcContractTest','TopologyQueryTest',
                                     'TopologyEngineMatrix','TopologyIdentityMatrix','DiscoveryTest','TopologyJsonStubTest']:
            raise AssertionError('Missing executable UT evidence: '+evidence)
python_counts={name:int(re.search(r'Ran (\d+) tests?',(out/(name+'.log')).read_text()).group(1))
               for name in ['python','source-audit','python-json']}
for name in ['nine-fixture-samples','biaoka-ubx','legacy-cpp']:
    assert 'PASS:' in (out/(name+'.log')).read_text()
summary=dict(scope=manifest['scope'], gtests=gtests, python=python_counts,
    udma=dict(checks=22,kind='assertion-based extracted OpenDevice/preparation/context injection CPU checks'),
    fixtures=dict(nine_input_sample_cases=len(selected),biaoka_ubx_cases=302,legacy_cases=2,
                  nine_full_pe_matrix='Historical result only; not rerun by this command'),
    requirements=requirements, all_executed_commands_passed=True, all_rfc_requirements_satisfied=False,
    pending=['Real CANN A2/A3/950 build and exported symbol/pybind linking',
             'Real init/finalize/failure rollback and memory import',
             'Hardware endpoint/channel/QP and XSCALE/HNS_1825',
             'CMO/quiet/shared-jetty data plane, IO DIE/plane and RoCE fabric connectivity',
             'Two-layer actual traffic/trace, latency/throughput/control-plane cost',
             'Generic arbitrary-QP planner differs from current fixed-two-QP demo'])
(out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
lines=['# RFC 全文 UT 与需求对应结果','',
       '结论：本轮可执行的 CPU UT/桩集成检查全部通过；不能据此宣布 RFC 全部设备要求已满足。',
       '', '需求基准：`'+str(a.rfc.resolve())+'`，SHA256：`'+manifest['rfc_sha256']+'`。',
       '代码基线：`'+manifest['git_head']+'` 加当前工作区，完整指纹见 [manifest](manifest.json)。',
       '', '## 本轮结果','', '| 检查 | 通过数 | 证据 |','| --- | ---: | --- |']
for g in gtests:
    lines.append('| '+g['xml']+' | '+str(g['passed'])+' | [XML]('+g['xml']+') |')
for n,c in python_counts.items(): lines.append('| '+n+' | '+str(c)+' | [日志]('+n+'.log) |')
lines += ['| UDMA/注入/OpenDevice | 22 | [日志](udma.log) |',
          '| 九份拓扑本轮样本 | '+str(len(selected))+' 场景 | [日志](nine-fixture-samples.log) |',
          '| biaoka/UBX | 302 场景 | [日志](biaoka-ubx.log) |',
          '| 旧 C++ 缓存注入 | 2 场景 | [日志](legacy-cpp.log) |','',
          'A2/A3 在 CPU 上分别运行同一非950宏分支，不是两类真实设备测试。Host/规划/UDMA C++ 检查启用 ASan/UBSan。',
          'Source-audit 仅检查接线/顺序/CMake规则，不等同真实构建。Python高层动态测试中native模块为桩；真实pybind复制仅有源码检查。',
          '多实例重复创建是fixture绑定/释放，不是完整init/finalize。kernel只提取分段算术，没有在NPU上执行传输。',
          '', '## 按 RFC 章节对应','',
          '`UT_PASS`：该条可本机验证的行为已执行；`UT_PARTIAL`：已有部分证据，限制见备注；`SOURCE_AUDIT`：仅静态检查；`NOT_RUN`：需要外部环境；`EVIDENCE`：追溯记录；`NOT_APPLICABLE`：规划信息。',
          '', '| RFC | 需求 | 结论 | 测试证据 | 限制 |','| --- | --- | --- | --- | --- |']
for r in requirements:
    lines.append('| '+r['section']+' | '+r['requirement']+' | '+r['status']+' | '+', '.join('`'+e+'`' for e in r['evidence'])+' | '+r['limitation']+' |')
lines += ['', '## 本轮未完成的设备要求','']+['- '+x for x in summary['pending']]
lines += ['', '## 与历史结果的关系','',
          '本轮新跑九份输入的源PE0和末源PE四种配置及失败样本，不宣称重新执行所有源目标对。此前3,441场景/256,716查询的完整矩阵保留在 [历史报告](../merged-1019-all/README.md)。',
          '本轮额外执行biaoka/UBX全302例。所有输入及模拟RootInfo保存在本目录；初始化失败单独记录，不伪装成成功查询。',
          '', '## 复现','', '```sh',
          'python3 -B tests/topology/run_rfc_ut.py --json /tmp/shmem-rfc-json-v3.11.3 --gtest /tmp/shmem-gtest-complete/googletest-1.14.0',
          '```','', '[全部命令回执](receipts.json) · [机器可读汇总](summary.json)']
(out/'README.md').write_text('\n'.join(lines)+'\n')
print(json.dumps(dict(host=[g['passed'] for g in gtests[:3]],planner=gtests[3]['passed'],
                     python=python_counts,fixtures=summary['fixtures'],output=str(out)),ensure_ascii=False),flush=True)
