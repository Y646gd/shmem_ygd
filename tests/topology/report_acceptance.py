#!/usr/bin/env python3
"""Verify uploaded-topology acceptance receipts and stream results into Excel.
No Python algorithm supplies C++ query or capability results.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import re
import zipfile
from xml.etree import ElementTree as ET
from xml.sax.saxutils import escape

ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent/'results/topo-acceptance-20261019')
args=parser.parse_args();out=args.output.resolve()
manifest=json.loads((out/'manifest.json').read_text())
receipts=json.loads((out/'run-receipts.json').read_text())
assert {r['name'] for r in receipts}==({'host-unit','legacy-cpp','python-wrapper','python-json'} | set(manifest['cases']))
assert all(r['returncode']==0 for r in receipts)
for receipt in receipts:
 assert receipt['input_fingerprint']==manifest['input_fingerprint']
 assert receipt['source_fingerprint']==manifest['source_fingerprint']
for item in manifest['inputs']:
 assert hashlib.sha256(Path(item['path']).read_bytes()).hexdigest()==item['sha256']
 assert hashlib.sha256(Path(item['synthetic_rootinfo']).read_bytes()).hexdigest()==item['synthetic_rootinfo_sha256']
for name,digest in manifest['source_sha256'].items():
 assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==digest,(name,'source changed since test')
fields=['record_type','platform','topology_label','case','engine_config','config_hex','source_pe','target_pe','query_mask','query_hex','stage_status','status','engine_mask','engine_hex','net_layer_count','net_layers','direct_stub','udma_qp_layers','check','expected_status','full_device_init','route_status','requested_qp_count','resolved_layers','hardware_qp_created']
kinds=['init_stage','query','validation','qp_route']
spools={kind:(out/(kind+'-records.jsonl')).open('w') for kind in kinds}
baselines={};basic_engines={};summaries=[];findings=collections.Counter();record_counts=collections.Counter()
csv_file=(out/'results.csv').open('w',newline='');writer=csv.writer(csv_file);writer.writerow(fields)
try:
 for platform in manifest['cases']:
  cases={c['name']:c for c in json.loads((out/(platform+'-cases.json')).read_text())['cases']}
  stages={};queries=collections.Counter();validations=collections.Counter();routes=0;query_success=0;last=''
  with (out/(platform+'.log')).open() as log:
   for line in log:
    last=line.strip()
    if not line.startswith('{'):continue
    r=json.loads(line);c=cases[r['case']];kind=r['record_type'];assert kind in kinds
    r.update(topology_label=c['topology_label'],engine_config=c['engine_config'],config_hex=hex(c['engine_config']),
        simulated=True,full_device_init=False)
    if kind=='init_stage':
     assert r['case'] not in stages
     assert r['stage_status']==c.get('expected_init_status',0),(r['case'],r['stage_status'])
     r['source_pe']=c['my_pe'];r['direct_stub']=c['direct_reachable']
     r['expected_status']=c.get('expected_init_status',0)
     stages[r['case']]=r
    elif kind=='query':
     stage=stages[r['case']];assert stage['stage_status']==0
     mask=r['query_mask'];peer=r['target_pe'];queries[r['case']]+=1
     r['query_hex']=hex(mask);r['direct_stub']=c['direct_reachable'][peer]
     r['udma_qp_layers']=stage['udma_qp_layers'][peer]
     expected_status=-100 if platform!='950' and mask&2 else 0
     assert r['status']==expected_status
     if expected_status:
      assert 'engine_mask' not in r and 'net_layers' not in r
     else:
      query_success+=1;r['engine_hex']=hex(r['engine_mask'])
      assert r['engine_state_source']=='production_capability_with_stub_D'
      assert r['net_layer_count']==len(r['net_layers'])
      assert r['net_layers']==sorted(set(r['net_layers']))
      assert all(layer in (0,1,3) for layer in r['net_layers'])
      if not mask&2 or peer==c['my_pe']:assert r['net_layers']==[]
      expected=(1 if c['direct_reachable'][peer] else 0)|(c['engine_config']&4)
      if platform!='950' and c['direct_reachable'][peer]:expected|=c['engine_config']&2
      if (platform=='950' and peer!=c['my_pe'] and c['direct_reachable'][peer] and c['engine_config']&8
          and c.get('transport_manager',True) and stage['udma_qp_layers'][peer]):expected|=8
      if not mask&1:expected=0
      assert r['engine_mask']==expected,(r['case'],peer,mask,r['engine_mask'],expected)
      if platform=='950':assert not r['engine_mask']&2
      if platform=='950' and mask==3 and '_basic' in r['case']:
       basic_engines[(c['topology_label'],c['engine_config'],c['my_pe'],peer)]=r['engine_mask']
      if platform=='950' and mask&2:
       key=(c['topology_label'],c['my_pe'],peer)
       if key in baselines:assert r['net_layers']==baselines[key],key
       else:baselines[key]=r['net_layers']
    elif kind=='validation':
     assert stages[r['case']]['stage_status']==0
     assert r['status']==r['expected_status'];validations[r['case']]+=1
    else:
     routes+=1
     assert r['hardware_qp_created'] is False
     if r['route_status']==0:
      assert len(r['resolved_layers'])==r['requested_qp_count']
      for layer in r['resolved_layers']:
       if layer not in (0,1):findings[(r['topology_label'],layer)]+=1
    spools[kind].write(json.dumps(r,ensure_ascii=False)+'\n');record_counts[kind]+=1
    writer.writerow([json.dumps(r.get(k),ensure_ascii=False) if isinstance(r.get(k),(list,dict)) else r.get(k,'') for k in fields])
  assert last.startswith('PASS: '+str(len(cases))+' C++ JSON cases'),last
  assert set(stages)==set(cases)
  for name,c in cases.items():
   if stages[name]['stage_status']:
    assert queries[name]==validations[name]==0
   else:
    assert queries[name]==len(c['direct_reachable'])*3 and validations[name]==10
  summaries.append(dict(platform=platform,cases=len(cases),successful_stages=sum(s['stage_status']==0 for s in stages.values()),
      expected_failed_stages=sum(s['stage_status']!=0 for s in stages.values()),query_calls=sum(queries.values()),
      query_success=query_success,validation_calls=sum(validations.values()),qp_route_observations=routes))
finally:
 for f in spools.values():f.close()
 csv_file.close()
unit_counts=[int(n) for n in re.findall(r'\[  PASSED  \] (\d+) tests',(out/'host-unit.log').read_text())];assert len(unit_counts)==2
assert (out/'legacy-cpp.log').read_text().splitlines()[-1].startswith('PASS: 2 C++ JSON cases')
for name in ('python-wrapper','python-json'):assert '\nOK\n' in (out/(name+'.log')).read_text()
init=(ROOT/'src/host/init/shmem_init.cpp').read_text()
assert init.index('init_manager->validate_peer_reachability(')<init.index('g_state.is_aclshmem_initialized = true;')
input_summaries=[]
for item in manifest['inputs']:
 name=item['name'];n=item['peers'];matrix=[[baselines[(name,i,j)] for j in range(n)] for i in range(n)]
 assert all(matrix[i][j]==matrix[j][i] for i in range(n) for j in range(n))
 # Independent post-run oracle for this explicit synthetic input model: raw
 # endpoint adjacency/attachment sets, not C++ output used as its own expected L.
 # It never populates the fixture or query cache, and says nothing about real D.
 physical=json.loads(Path(item['path']).read_text())
 ids=[peer['local_id'] for peer in physical['peer_list']]
 mesh=collections.Counter();attachments=collections.defaultdict(set);roce=set()
 for edge in physical['edge_list']:
  if 'local_b' in edge:
   if edge.get('local_a_ports') and edge.get('local_b_ports'):
    mesh[tuple(sorted((edge['local_a'],edge['local_b'])))]+=1
  elif edge.get('local_a_ports'):
   layer=edge['net_layer']
   attachments[(layer,edge.get('topo_instance_id',0))].add(edge['local_a'])
   if layer==3:roce.add(edge['local_a'])
 merged=bool(mesh) and any(layer==0 for layer,_ in attachments)
 for i,a in enumerate(ids):
  for j,b in enumerate(ids):
   expected=set()
   if i!=j:
    if tuple(sorted((a,b))) in mesh:expected.add(0)
    for (raw_layer,_),members in attachments.items():
     if raw_layer in (0,1) and a in members and b in members:
      expected.add(1 if raw_layer==0 and merged else raw_layer)
    if a in roce and b in roce:expected.add(3)
   if item.get('input_kind')=='original_rootinfo':
    # The original biaoka RootInfo advertises only layers 0 and 3.
    expected.discard(1)
   assert matrix[i][j]==sorted(expected),(name,i,j,matrix[i][j],sorted(expected))
 counts=collections.Counter(tuple(v) for row in matrix for v in row)
 input_summaries.append(dict(name=name,peers=n,edges=item['edges'],sha256=item['sha256'],
    mesh_edge_entries=sum(mesh.values()),mesh_unique_pairs=len(mesh),mesh_extra_entries=sum(mesh.values())-len(mesh),
    raw_graph_oracle_checked=True,
    layer_sets={str(list(k)):v for k,v in sorted(counts.items())},full_pe_pairs=n*n,matrix=matrix,
    basic_engine_matrices={str(cfg):[[basic_engines[(name,cfg,i,j)] for j in range(n)] for i in range(n)] for cfg in (1,5,9,13)}))
# Identical physical files must yield identical layer matrices despite different synthetic names.
identical_inputs=[]
for i,a in enumerate(input_summaries):
 for b in input_summaries[i+1:]:
  if a['sha256']==b['sha256']:
   assert a['matrix']==b['matrix']
   identical_inputs.append([a['name'],b['name']])
summary=dict(scope='LOCAL_CPU_ONLY',device_acceptance='NOT_RUN_USER_CHOICE',
    rootinfo=manifest['rootinfo_policy'],input_directory=manifest['input_source_directory'],
    platforms=summaries,host_unit_tests=unit_counts,record_counts=dict(record_counts),
    inputs=input_summaries,source_sha256=manifest['source_sha256'],
    identical_input_pairs=identical_inputs,
    qp_layers_outside_setter_scope=[dict(topology=k[0],layer=k[1],observed_qp_entries=n) for k,n in sorted(findings.items())])
(out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2))
template=(Path(__file__).resolve().parent/'acceptance_report_template.html').read_text()
if manifest.get('suite')=='950-biaoka-ubx':
 template=template.replace('九份上传拓扑','950 biaoka / UBX').replace('只使用 tests/topology/topo 中的九份文件。','使用原始 biaoka RootInfo 与 UBX 物理拓扑。').replace('所有 RootInfo 的地址、plane、网络实例均为模拟。','biaoka 保留原始 RootInfo；UBX 地址、plane、网络实例为模拟。').replace('九份拓扑完整本机验收结果.xlsx','950-biaoka-UBX验收结果.xlsx')
(out/'report.html').write_text(template.replace('__DATA__',json.dumps(input_summaries,ensure_ascii=False).replace('</','<\\/')))

# Stream worksheet XML into ZIP; the full matrix can exceed 200k query rows.
NS='http://schemas.openxmlformats.org/spreadsheetml/2006/main'
def col(n):
 text=''
 while n:n,k=divmod(n-1,26);text=chr(65+k)+text
 return text

def record_rows(kind):
 yield fields
 with (out/(kind+'-records.jsonl')).open() as stream:
  for line in stream:
   r=json.loads(line);yield [r.get(f,'') for f in fields]

def export_xlsx():
 summary_rows=[['项目','结果'],['范围','新上传九份文件；只完成本机CPU验证'],['RootInfo','九份均模拟；地址、plane、网络实例假设见README'],['Host UT',unit_counts]]
 if manifest.get('suite')=='950-biaoka-ubx':
  summary_rows[1:3]=[['范围','950 biaoka / UBX；本机CPU验证'],['RootInfo',manifest['rootinfo_policy']]]
 summary_rows += [[s['platform'],s] for s in summaries]
 input_rows=[['文件','PE数','边数','输入SHA256','完整PE对数','LINKS集合与有向对数','Mesh条目数','不同Mesh端点对','额外并行Mesh条目','原边投影断言通过']]+[[s[k] for k in ('name','peers','edges','sha256','full_pe_pairs','layer_sets','mesh_edge_entries','mesh_unique_pairs','mesh_extra_entries','raw_graph_oracle_checked')] for s in input_summaries]
 pending=[['项目','状态'],['A2/A3/950完整CANN构建、符号与pybind','NOT_RUN'],['真实D、跨机/超节点/段导入','NOT_RUN'],['真实RootInfo、地址、plane、IO DIE与RoCE全互通','NOT_RUN'],['UDMA硬件QP/channel与XSCALE/HNS_1825','NOT_RUN'],['CMO/quiet数据面及完整init失败回滚','NOT_RUN']]
 observations=[['项目','说明'],['直接状态输入','D和基础UDMA路由是显式桩条件；QP策略场景使用实际C++选路生成路由描述。'],['非950分支','A2/A3为合同回归，不表示Atlas产品的实际SoC身份。'],['缓存优化','仅缓存不可变的远端bootstrap payload；本地Initialize、能力生成、层缓存和public query每例执行。']]
 for pair in identical_inputs:observations.append(['相同内容的独立输入',pair])
 for finding in summary['qp_layers_outside_setter_scope']:observations.append(['默认选路出现setter范围外层',finding])
 sheets=[('验收汇总',lambda:iter(summary_rows)),('输入与层结果',lambda:iter(input_rows))]
 sheets += [(title,lambda kind=kind:record_rows(kind)) for title,kind in [('初始化阶段','init_stage'),('查询结果','query'),('错误参数验收','validation'),('QP选路观察','qp_route')]]
 sheets += [('边界与发现',lambda:iter(observations)),('设备待验收',lambda:iter(pending))]
 contents='<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>'+''.join(f'<Override PartName="/xl/worksheets/sheet{i}.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>' for i in range(1,len(sheets)+1))+'</Types>'
 relationships='http://schemas.openxmlformats.org/package/2006/relationships';office='http://schemas.openxmlformats.org/officeDocument/2006/relationships'
 files={'[Content_Types].xml':contents,'_rels/.rels':f'<Relationships xmlns="{relationships}"><Relationship Id="rId1" Type="{office}/officeDocument" Target="xl/workbook.xml"/></Relationships>',
  'xl/workbook.xml':f'<workbook xmlns="{NS}" xmlns:r="{office}"><sheets>'+''.join(f'<sheet name="{name}" sheetId="{i}" r:id="rId{i}"/>' for i,(name,_) in enumerate(sheets,1))+'</sheets></workbook>',
  'xl/_rels/workbook.xml.rels':f'<Relationships xmlns="{relationships}">'+''.join(f'<Relationship Id="rId{i}" Type="{office}/worksheet" Target="worksheets/sheet{i}.xml"/>' for i in range(1,len(sheets)+1))+f'<Relationship Id="rId{len(sheets)+1}" Type="{office}/styles" Target="styles.xml"/></Relationships>',
  'xl/styles.xml':f'<styleSheet xmlns="{NS}"><fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><b/><sz val="11"/><name val="Calibri"/></font></fonts><fills count="2"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill></fills><borders count="1"><border/></borders><cellStyleXfs count="1"><xf/></cellStyleXfs><cellXfs count="2"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/><xf numFmtId="0" fontId="1" fillId="0" borderId="0" xfId="0"/></cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>'}
 path=out/('950-biaoka-UBX验收结果.xlsx' if manifest.get('suite')=='950-biaoka-ubx' else '九份拓扑完整本机验收结果.xlsx');sheet_counts=[]
 with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as z:
  for name,body in files.items():z.writestr(name,'<?xml version="1.0" encoding="UTF-8"?>'+body)
  for index,(_,factory) in enumerate(sheets,1):
   width=0;row_number=0;chunks=[];size=0
   with z.open(f'xl/worksheets/sheet{index}.xml','w') as f:
    f.write((f'<?xml version="1.0" encoding="UTF-8"?><worksheet xmlns="{NS}"><sheetViews><sheetView workbookViewId="0"><pane ySplit="1" topLeftCell="A2" activePane="bottomLeft" state="frozen"/></sheetView></sheetViews><cols><col min="1" max="30" width="24" customWidth="1"/></cols><sheetData>').encode())
    for row_number,row in enumerate(factory(),1):
     width=max(width,len(row));cells=[f'<row r="{row_number}">']
     for j,value in enumerate(row,1):
      if value=='':continue
      ref=f'{col(j)}{row_number}';style=1 if row_number==1 else 0
      if isinstance(value,int) and not isinstance(value,bool):cells.append(f'<c r="{ref}" s="{style}"><v>{value}</v></c>')
      else:
       value=json.dumps(value,ensure_ascii=False) if isinstance(value,(list,dict)) else str(value)
       cells.append(f'<c r="{ref}" s="{style}" t="inlineStr"><is><t xml:space="preserve">{escape(value)}</t></is></c>')
     cells.append('</row>');block=''.join(cells).encode();chunks.append(block);size+=len(block)
     if size>1024*1024:f.write(b''.join(chunks));chunks=[];size=0
    if chunks:f.write(b''.join(chunks))
    f.write(f'</sheetData><autoFilter ref="A1:{col(width)}{row_number}"/></worksheet>'.encode())
   sheet_counts.append(row_number)
 with zipfile.ZipFile(path) as z:
  assert z.testzip() is None
  for index,expected in enumerate(sheet_counts,1):
   count=0
   with z.open(f'xl/worksheets/sheet{index}.xml') as f:
    for _,element in ET.iterparse(f,events=('end',)):
     if element.tag==f'{{{NS}}}row':count+=1;element.clear()
   assert count==expected
 return path,sheet_counts
excel,sheet_counts=export_xlsx()
text=['# 九份新上传拓扑：完整本机验收结果（2026-10-19）','',
 '本次只使用tests/topology/topo中的九份上传文件。上一轮acceptance-20261019生成目录已按要求删除；不混入此前biaoka/UBX或Driver目录的输入。通用回归中的旧fixture仅为辅助兼容测试，不纳入九份输入关系矩阵。',
 '', '用户已确认沿用模拟RootInfo。九份原物理JSON不写入；mock地址/plane/fabric显式生成，D和UDMA基础路由是测试条件。CANN/NPU设备验收NOT_RUN。','',
 '| 平台分支 | 场景 | 成功阶段 | 预期失败阶段 | query结果记录 | 错误参数记录 | QP选路观察 |','| --- | --- | --- | --- | --- | --- | --- |']
for s in summaries:text.append('| '+' | '.join(str(s[k]) for k in ['platform','cases','successful_stages','expected_failed_stages','query_calls','validation_calls','qp_route_observations'])+' |')
text+=['',f'Host两种宏分支各{unit_counts}项通过；原C++桩2例与Python回归通过。所有C++启用ASan/UBSan。A2/A3仅为公开合同分支回归；主输入按950上下文测试，不由产品文件名推断SoC。','',
 '| 上传文件 | PE | 边 | LINKS集合：有向PE对数量 |','| --- | --- | --- | --- |']
for s in input_summaries:text.append(f"| {s['name']}.json | {s['peers']} | {s['edges']} | {s['layer_sets']} |")
for pair in identical_inputs:text.append('\n输入 '+ ' / '.join(pair)+' 内容SHA相同；分别执行并验证完整层关系矩阵一致，未合并或修改原文件。')
for s in input_summaries:
 if s['mesh_extra_entries']:text.append(f"\n{s['name']}有{s['mesh_edge_entries']}条Mesh记录，但只覆盖{s['mesh_unique_pairs']}个不同无向端点对，另有{s['mesh_extra_entries']}条并行端口记录。不能把记录数当成互异PE对数；本轮按原边逐对核验。")
text+=['','所有源PE的MTE/MTE+ROCE/MTE+UDMA/MTE+UDMA+ROCE基本组合均逐目标查询；另外从源PE0覆盖1..15配置×D/U/manager边界及预期失败。QP策略覆盖1/2/4、default/layer0/layer1/mixed、multi-link开关。网络层集合跨配置保持一致、排序去重、self为空；完整矩阵对称。后处理还在明确的模拟RootInfo假设下，用原始Mesh端点对、Clos附件域及RoCE附件构成独立期望，逐个PE对核对L；该期望没有供给C++生产或查询缓存，不能推断真实D。',
 '', '950 D=1时，MTE+SDMA→0x1，MTE+ROCE→0x5，MTE+SDMA+ROCE→0x5；950返回不含SDMA D2D位。配置0xb/0xf还受实际测试QP0路由条件约束。失败阶段与query分开，失败不发布initialized，C++额外验证public query返回-5。',
 '', '## 实现链路','',
 '```text\n上传九份原TopoInfo（记录SHA256）\n → 每份生成独立模拟RootInfo（原边保留）\n → 真实C++ ParseRootInfo/ParseTopoInfo/BuildLocalEndpoints\n → bootstrap桩回放；真实TopologyContext::Initialize\n → 生产配置转换和LoadExtendLibrary函数体（动态加载桩）\n → CanReachDataOperators（D、manager/路由明确为桩）\n → reach_info_init的装配片段 → topo_list\n → 生产validate_peer_reachability → 成功才发布测试实例\n → build_topology_state/GetReachableNetLayers\n → aclshmemx_query_topology → backend::query_topology\n → C++断言/日志 → 追加核验 → CSV/JSON/Excel\n```',
 '', 'PreparePeers缓存只保存不可变的远端bootstrap输入，不缓存引擎能力、L或query返回。每个场景重新执行本地Initialize、能力生成、门禁、层缓存和公开查询。QP策略使用实际C++GetEidRoutes，并用生产QP层getter函数体读取最小peer_routes容器；没有创建硬件QP。',
 '', '模拟假设：PE=device=local_id；每个原端口生成模拟EID（layer3为192.0.2.0/24 IPv4）；每层同一非空测试plane和网络实例；layer3声明全互通。layer2物理边保留，不进入公共LINKS集合；默认QP选路是否引用该模拟层另列观察，不能视为设备支持证明。',
 '', '## 需要区分的观察','']
if findings:
 for (name,layer),count in sorted(findings.items()):text.append(f'- {name}：默认/策略选路输出中观察到layer {layer}，共{count}个QP条目。模拟RootInfo为该物理层补了EID，因此这仅是当前选路函数对假设输入的输出；公开setter仍拒绝layer2/3。这不计为该层硬件支持验收。')
else:text.append('本轮QP选路观察未出现setter支持范围外的层。')
text+=['', '完整设备构建/符号/pybind、真实地址与plane、实际D/段导入、跨机同超/跨超、UDMA硬件QP、XSCALE/HNS_1825、CMO/quiet和完整init回滚仍为NOT_RUN。初始化阶段记录执行生产函数体及状态门禁，不是完整aclshmemx_init_attr设备运行。',
 '', '[交互接口结果](report.html) · [Excel](九份拓扑完整本机验收结果.xlsx) · [JSON汇总及完整层矩阵](summary.json) · [完整CSV](results.csv) · [命令回执](run-receipts.json) · [输入清单与SHA](manifest.json)',
 '', '复现：从仓库根目录执行 `python3 tests/topology/run_acceptance.py --topo-dir tests/topology/topo`，再运行 `python3 tests/topology/report_acceptance.py`。--json/--gtest可指定依赖；--resume只在输入与源码指纹均一致时复用成功回执。']
if manifest.get('suite')=='950-biaoka-ubx':
 text[0]='# 950 biaoka / UBX 独立验收结果'
 text[2]='本次为独立新增目录，保留此前九份拓扑验收目录。biaoka 使用原始 root_info-biaoka.json 及 atlas_350_3.json；UBX 使用 root_info-ubx.json 的原物理边，并生成模拟 RootInfo。'
 text[4]='biaoka 保留原始地址、plane 和网络实例；UBX 使用用户批准的模拟 RootInfo。两者 D/基础 UDMA 路由均为显式桩，设备验收 NOT_RUN。'
 text=[line.replace('九份拓扑完整本机验收结果.xlsx','950-biaoka-UBX验收结果.xlsx').replace('九份输入','两份输入').replace('上传九份原TopoInfo','biaoka 原RootInfo及物理边 / UBX 原物理边').replace('每份生成独立模拟RootInfo','biaoka保留原RootInfo；UBX生成模拟RootInfo') for line in text]
 text=[line.replace('模拟假设：PE=device=local_id；','仅 UBX 的模拟假设：PE=device=local_id；').replace('在明确的模拟RootInfo假设下，用原始Mesh端点对、Clos附件域及RoCE附件构成独立期望','用原物理边构成独立期望，biaoka 限定为原 RootInfo 已声明的 0/3 层，UBX 按模拟 RootInfo 的网络域假设') for line in text]
 text=[line.replace('| biaoka.json |','| root_info-biaoka.json + atlas_350_3.json |').replace('| UBX.json |','| root_info-ubx.json |') for line in text]
 text[-1]='复现：仓库根目录执行 `python3 tests/topology/950_biaoka_ubx/run.py`。完整链路及参数见该目录 README.md。'
(out/'README.md').write_text('\n'.join(text)+'\n')
print(json.dumps({'platforms':summaries,'host_tests':unit_counts,'records':record_counts,'input_layers':[{k:v for k,v in i.items() if k not in ('matrix','basic_engine_matrices')} for i in input_summaries],'qp_observations':summary['qp_layers_outside_setter_scope'],'excel':str(excel),'excel_sheet_rows':sheet_counts},ensure_ascii=False,indent=2))
