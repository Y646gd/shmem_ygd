# 九份上传拓扑的完整本机验收与执行链路

1019 迁移后的入口、合并策略及新结果见 [README_1019_MERGE.md](README_1019_MERGE.md)。下文保留原验收流程说明；旧结果目录不随迁移复制。

本次主输入固定为 [topo/](topo/) 中九份用户上传的原始JSON。上一轮results/acceptance-20261019生成产物已按要求删除；实现、通用测试入口、20261016历史结果与checkpoint提交保留。本轮不从driver目录或此前biaoka/UBX文件生成主验收场景。

用户选择沿用模拟RootInfo，明确只算本机验证；完整CANN/NPU验收仍为NOT_RUN。文件名Atlas350/850/950是产品描述，不能直接当作SoC型号；九份主输入按950上下文测试，A2/A3运行作为公开合同宏分支回归，不代表这些产品属于A2/A3。

## 运行与输出

从当前仓库根目录执行：

```bash
python3 tests/topology/run_acceptance.py --topo-dir tests/topology/topo
python3 tests/topology/report_acceptance.py
```

默认结果目录：[results/topo-acceptance-20261019/](results/topo-acceptance-20261019/)。其中包括九份输入清单与SHA256、生成的独立模拟RootInfo、A2/A3/950用例、C++日志、逐命令回执、分类型JSONL、CSV、层关系矩阵与Excel。

依赖路径可用--json和--gtest指定；默认使用本机已准备的固定版本nlohmann/json 3.11.3和GoogleTest 1.14.0。--generate-only仅生成fixture；--resume仅在九份输入与源码指纹均完全一致时复用成功回执。本轮从新目录完整运行，不复用上一轮结果。

## 本次覆盖

| 部分 | 覆盖 |
| --- | --- |
| 每份文件的全PE关系 | 950基本配置0x1/0x5/0x9/0xd；每个源PE到所有目标，mask=1/2/3；包括64 PE文件 |
| 配置与边界矩阵 | 每份源PE0，配置1..15，D=0/1、QP0有/无、manager缺失；含未显式MTE和CMO组合0x3/0x7/0xb/0xf |
| 预期失败 | 非950 UDMA扩展拒绝、D=0无RDMA、动态加载失败桩、远端失败汇合、bootstrap失败 |
| 查询错误 | 未初始化、空地址、非法PE、零/未知mask、布局不足、ABI错误，失败不消费输出字段 |
| QP策略与物理L独立性 | 每份源PE0，1/2/4条路由，default/layer0/layer1/mixed，multi-link开/关；对照L不变 |
| 通用回归 | Host UT两种宏分支、八种L、plane/端口/RoCE负例、身份、CMO quiet掩码、QP公开setter/getter函数体、旧C++桩2例与Python回归 |

D是显式测试条件，不从Mesh边或LINKS推断。950禁止将SDMA CMO位算作D2D引擎。U要求非self、D=1、manager存在并由QP0 getter实际返回路由；基础矩阵的路由描述显式打桩，QP策略场景则由真实TopoQuerier::GetEidRoutes产生描述。没有Hcomm endpoint/channel或硬件QP。

## 实际实现链路与代码

```text
九份上传原TopoInfo（不修改，记录SHA256）
  → run_acceptance.py：每份生成模拟RootInfo和独立预期矩阵
  → cpp_json_stub/run.py：定义ACLSHMEM_SOC_950或非950合同分支
      直接编译topo_reader.cpp、topology_context.cpp
      提取生产配置转换、LoadExtendLibrary、CanReachDataOperators、位装配
      提取生产validate_peer_reachability、public/backend query、build
  → cpp_json_stub/main.cpp：逐用例
      ParseRootInfo/ParseTopoInfo/BuildLocalEndpoints生成bootstrap输入
      TopologyContext::Initialize本地发现 + replay allgather
      配置转换 → 扩展库加载函数体（DlApi加载桩）
      CanReachDataOperators（D及资源条件为桩）
      StoreCapability → topo_list
      validate_peer_reachability（生产函数体，非self零能力→-100，状态汇合）
      成功才build_topology_state并发布测试initialized
      aclshmemx_query_topology → backend::query_topology
      status / engine_mask / net_layer_count / net_layers → C++断言和日志
  → report_acceptance.py：独立字段核验、L不变/对称/去重、自环、指纹检查
      流式CSV/JSONL/Excel，九份文件分别汇总
```

代码入口：

- [run_acceptance.py](run_acceptance.py)：用例与模拟输入生成、编译执行回执。
- [cpp_json_stub/run.py](cpp_json_stub/run.py)：生产源码提取和宏分支编译。
- [cpp_json_stub/main.cpp](cpp_json_stub/main.cpp)：bootstrap桩、能力链、失败状态与查询断言。
- [生产能力生成](../../src/host/entity/mem_entity_default.cpp)。
- [生产位装配、门禁与查询](../../src/host/init/backends/shmem_init_backend.cpp)。
- [public query及初始化门禁调用位置](../../src/host/init/shmem_init.cpp)。
- [生产发现与交换](../../src/host/transport/topo/topology_context.cpp)。
- [生产解析、选路与层匹配](../../src/host/transport/topo/topo_reader.cpp)。
- [report_acceptance.py](report_acceptance.py)：结果核验及流式Excel导出。

对于同一份输入，PreparePeers只缓存不可变远端bootstrap payload（避免64 PE多配置下重复解析所有远端输入）。缓存键包含完整RootInfo、devices和原TopoInfo内容；每次返回独立replay副本。它不缓存引擎能力、公共query返回或L；本地Initialize、能力生成、门禁、build和query每例重新执行。

生产初始化的门禁在setup_heap之后、memory_manager_initialize和initialized发布之前，固定栈数组allgather同步非self远端零能力；使用既有init guard退出。此测试只执行该函数体和相关阶段，不运行完整aclshmemx_init_attr；所以日志称init_stage而不是设备init成功。

## 模拟RootInfo的边界

九份文件仅提供peer_list/edge_list，没有真实地址/plane/网络实例。生成器采用明确假设：PE=device_id=local_id；保留每条原边和端口，给每个端口分配模拟EID（layer3使用192.0.2.0/24 IPv4）；每层同一非空测试plane/fabric，layer3声明全互通。原始JSON不写入，生成文件带synthetic-rootinfo后缀。缺少真实RootInfo时，不证明这些假设符合产品实机。

原物理layer2边保留。公共LINKS仅返回0/1/3；如果默认路由在包含模拟EID的layer2上产生条目，报告另列为选路观察，不能当成硬件支持或layer2公共setter支持。公开setter仍按原合同拒绝2/3。

A2/A3便携runner为了加载fixture仍编译TopoReader；这不是非950正式构建引入拓扑模块的证据。两个平台能力/查询宏分支共用，但身份差异由生产IsSdmaAccessible函数体的CPU测试单独覆盖。

## 尚待设备验收（NOT_RUN）

- 完整A2/A3/950 CANN构建、动态库符号、pybind与集成单测。
- 实际单机同组/跨组、跨机同超/跨超、设备身份及内存段导入D。
- 真实RootInfo、IO DIE/plane/聚合端口与RoCE全互通范围。
- 实际UDMA QP/channel、完整get_qp_net_layer调用链、XSCALE/HNS_1825资源与失败。
- CMO/quiet数据面、完整init失败回滚、多进程故障和重复init/finalize。

## 结果解释

D=1且资源可用的950条件下，MTE+SDMA→0x1，MTE+ROCE→0x5，MTE+SDMA+ROCE→0x5。LINKS-only返回engine_mask=0；无远端引擎则初始化阶段预期失败，不发布可成功查询实例。完整记录包含明确的D、QP路由输入、平台合同分支与配置字段，不能把它解释为从拓扑JSON测得的真实硬件引擎能力。

结果中“预期失败”表示负例按合同通过；“query结果记录”包含A2/A3请求LINKS时的预期-100，不等于全部调用status=0。真实初始化及数据通信没有执行。

## 950 biaoka / UBX 独立复测

另见 [独立目录、复现入口和完整实现链路](950_biaoka_ubx/README.md)。保留本目录九份输入结果；biaoka 使用原 RootInfo，UBX 使用模拟 RootInfo，均为本机验收。
