# 950 JSON 拓扑打桩测试（临时，TEMP_TOPOLOGY_JSON_STUB）

## Mac 本地运行（推荐入口，无服务器依赖）

只需 Python 3 标准库，无需昇腾服务器、CANN、NPU、CMake、GoogleTest、第三方 Python 包，也无需联网。以下命令从仓库根目录执行：

```bash
# 读取 JSON，打印本 PE 到所有目标 PE 的引擎和 LINKS
python3 -B tests/topology/json_stub/local_stub.py

# 只查目标 PE1
python3 -B tests/topology/json_stub/local_stub.py --pe 1

# 指定场景和查询位：1=ENGINES、2=LINKS、3=两者
python3 -B tests/topology/json_stub/local_stub.py --case mte_only_links_unchanged --pe 1 --mask 3

# 使用自己定义的套件
python3 -B tests/topology/json_stub/local_stub.py --fixtures /absolute/path/cases.json --case my_case

# 执行本地测试（包含内置 11 个 JSON 场景）
python3 -B -m unittest discover -s tests/topology/json_stub -p 'test_local_stub.py' -v
```

PE1 的默认输出包含：

```json
{"mode":"local_python_stub","case":"mixed_engines_and_layers","source_pe":0,"target_pe":1,"engine_mask":15,"net_layer_count":3,"net_layers":[0,1,3]}
```

本地接口：`JsonTopologyStub(case, directory)` 读取文件并建立快照，`query_topology(pe, query_mask=3)` 返回上述三个结果字段，`finalize()` 结束实例。结果列表为独立副本。输入错误抛异常，CLI 返回非零。`expected_*` 字段只用于测试断言，查询算法不会读取它们。

本地流程：JSON → 本地描述解析 → 按正式 C++ 规则建模的物理匹配 → 实例缓存 → 本地查询。实现覆盖 Mesh、Clos、合并 raw layer 0 归一化和 RoCE 3；ENGINES 来自输入的模拟可达位。

**Mac 入口是 Python 行为模型，没有调用正式 C++ 动态库，也不验证 C++ ABI、真实设备发现或资源建链。** 它用于离线构造场景和观察查询结果；下面保留的 C++ UT 是可选的正式实现验证入口。Python 模型在正式 C++ 算法变化时需要同步维护。

## 两种测试的边界

Mac 入口通过 Python 行为模型离线返回查询结果；可选 C++ UT 用同一批自定义 JSON 替代真实设备发现和跨 PE 描述交换，再调用正式的物理层匹配、实例缓存构建和 `aclshmemx_query_topology()`。

这些入口不是运行库的新 public API。仅可选 C++ UT 的编译/链接需要项目原有的 CANN、JSON、GoogleTest 等依赖及 UT mock 环境；Mac Python 入口不需要这些依赖。不执行 `aclshmemx_init_attr()`、heap 分配、真实 bootstrap 或数据搬运，不写 `/etc/hccl_rootinfo.json`。

ENGINES 的硬件发现是桩：`internal_transport_masks` 模拟初始化后 `topo_list` 的值。JSON 端口不能证明引擎可用。LINKS 则由真实算法计算，不能把 `expected_layers` 当成查询返回值注入。此测试不能替代设备发现、真实 allgather、QP 建链、链路健康或实机通信验收。

## 可选 C++ UT 的调用链和结果保存位置

```text
LoadTopologyJsonStub(cases.json 中的一条用例)
  ├─ 读取 RootInfo JSON
  ├─ ParseRootInfoJsonForTest() → 正式 ParseRootInfoJson()
  ├─ 正式 ParseTopoInfo()：打开 topology 指定的拓扑文件
  ├─ 正式 GetLocalId()/GetEidCount()/BuildLocalEndpoints()
  └─ 为每个 PE 构造 context，替代 runtime 与 endpoint allgather
       ↓
backend.bind_aclshmem_entity()：登记测试实例
state.topo_list[pe] = internal_transport_masks[pe]
backend.install_topology_context_for_test()：移交 context 所有权给实例
backend.build_topology_state()：真实 GetReachableNetLayers()，生成实例缓存
aclshmemx_query_topology()：真实公开接口，查询三个 mask 并比对预期
```

context 保存于 `entity_member::topology_context`；层集合保存于 `topology_net_layers_by_pe`。桩 allgather 只供 build 阶段 status agreement 使用，复制本地 status 到各 PE；没有测试真实 collective。

## 可选 C++ UT：有 CANN 开发环境时构建和运行

从仓库根目录，在已配置 CANN 和第三方依赖的 Linux 开发环境执行：

```bash
bash scripts/build.sh -soc_type Ascend950 -uttests
./build/bin/aclshmem_unittest --gtest_filter='TopologyJsonStubTest.*'
```

若本地构建输出目录不同，以生成的 `aclshmem_unittest` 路径为准。测试在 `host/init/`，开启 RDMA 后也仍属于主 `aclshmem_unittest`，不是 `aclshmem_rdma_topo_unittest`。新增测试通过既有 CMake GLOB 收集；添加文件后需重新配置/构建。只有同时定义 `ACLSHMEM_UNIT_TEST` 和 `ACLSHMEM_SOC_950` 才启用测试及注入接口。

默认读取本目录 `cases.json`。指定自己的套件（建议用绝对路径）：

```bash
ACLSHMEM_TOPOLOGY_FIXTURE=/absolute/path/cases.json \
  ./build/bin/aclshmem_unittest --gtest_filter='TopologyJsonStubTest.*'
```

输出示例格式（实际输出由 C++ 查询产生）：

```text
[JSON_STUB] mixed_engines_and_layers PE 0 -> 1 query=3 engines=15 layers=[0,1,3]
```

无需 CANN，可先校验 JSON 语法、schema 和预期值的编码：

```bash
python3 tests/topology/json_stub/check_fixtures.py
python3 tests/topology/json_stub/check_fixtures.py /absolute/path/cases.json
```

Python 检查器不会执行 C++ 匹配，也不能证明查询通过。

## 如何定义自己的 JSON

一份套件包含三个部分：`cases.json`、RootInfo JSON、拓扑 JSON。所有文件引用均相对于 `cases.json` 所在目录，也支持绝对路径。

用例字段示例：

```json
{
  "cases": [{
    "name": "my_case",
    "root_info": "rootinfo.json",
    "topology": "topo.json",
    "devices": [10, 11, 12, 13],
    "my_pe": 0,
    "internal_transport_masks": [0, 15, 2, 4],
    "expected_engine_masks": [0, 15, 4, 2],
    "expected_layers": [[], [0, 1, 3], [1], [3]],
    "parse_ok": true
  }]
}
```

- `devices[pe]`：该 PE 的物理设备 ID，对应 RootInfo `rank_list[].device_id`。不要求与 `local_id` 或 PE 编号相等。
- `my_pe`：查询源 PE。预期数组按目标 PE 索引，长度等于 devices 数量；更改源 PE 时也要更新预期。
- `internal_transport_masks`：**内部 transport 位**，MTE=1、ROCE=2、SDMA=4、UDMA=8，可按位或。
- `expected_engine_masks`：**公开 engine 位**，MTE=1、SDMA=2、ROCE=4、UDMA=8。ROCE 与 SDMA 的位置不同。
- `expected_layers`：物理描述预期产生的排序去重集合，只允许 0/1/3；源 PE 查询自身应为空。
- `parse_ok=false`：期望描述被业务解析拒绝；此类用例不进入缓存构建和查询。

RootInfo 直接沿用现有格式：`topo_file_path`、`rank_list`、`device_id`、`local_id`、`level_list`、`net_type`、`net_layer`、`net_instance_id`、`rank_addr_list`、`addr_type`、`addr`、`plane_id`、`ports`。参照 `rootinfo.json` 修改；本套件用合法 IP 地址测试物理关系，不要求伪造真实 EID。也可以使用正式 parser 支持的 EID 描述。

RootInfo 的 `topo_file_path` 仍为必需字段；解析后，桩用用例 `topology` 路径覆盖它，便于文件整体搬移。正式 ParseTopoInfo 的路径检查仍执行。

拓扑文件 `edge_list` 中：Mesh 条目提供 `local_a/local_b` 及两端端口；Clos 条目提供 `local_a/net_layer/topo_instance_id/local_a_ports`。网络实例边界在 RootInfo 中，topology instance 在拓扑附件中，两者不要混淆。

## 内置用例

| 用例 | 重点 |
| --- | --- |
| mixed_engines_and_layers | peer1 为 0/1/3，peer2 为 1，peer3 为 3；检查 ROCE/SDMA 位转换 |
| mte_only_links_unchanged | 只模拟 MTE 使能，物理 LINKS 保持不变 |
| no_enabled_engines_links_unchanged | 引擎集合为空，物理 LINKS 仍可存在 |
| udma_only_links_unchanged | 模拟 UDMA-only，LINKS 不由使能位决定 |
| clos_plane_mismatch | peer1 Clos plane 不匹配，移除 layer1 |
| mesh_domain_mismatch | peer1 Mesh 网络实例不同，移除 layer0 |
| roce_domain_mismatch | RoCE 网络实例不同，移除 layer3 |
| dual_plane_deduplicated | 同层左右两 plane 可达，layer1 只返回一次 |
| roce_address_family_mismatch | 两端 IPv4/IPv6 不同，移除 layer3 |
| missing_required_root_fields | 缺失 RootInfo 必需字段，解析失败 |
| empty_topology | 无有效边，解析失败 |

每个正例对所有目标 PE 查询 mask=1、2、3；检查未请求组为零/空、自身 LINKS 为空。原始 Mesh 文件还包含一条无效候选边，用于确认会遍历后续有效边。

## 正式版本删除清单

只删除下列新增内容，保留原有 topology 功能：

1. 删除 `tests/topology/json_stub/` 整个目录（JSON、校验器、本文）。
2. 删除 `tests/unittest/host/init/topology_json_stub_test.cpp`。
3. 在 `src/host/transport/topo/topo_reader.h` 删除带 `TEMP_TOPOLOGY_JSON_STUB` 标记的 guarded `ParseRootInfoJsonForTest()` 包装。
4. 在 `src/host/init/backends/shmemi_init_backend.h` 删除 guarded `install_topology_context_for_test()` 声明。
5. 在 `src/host/init/backends/shmem_init_backend.cpp` 删除该函数的 guarded 定义。

定位命令：

```bash
rg -n 'TEMP_TOPOLOGY_JSON_STUB|ParseRootInfoJsonForTest|install_topology_context_for_test|ACLSHMEM_TOPOLOGY_FIXTURE' src tests
```

没有修改 public headers、公开查询 ABI、运行库发现入口或 CMake；无环境变量会启用正式运行库桩。C++ 注入接口仅在 UT 目标中存在，正式运行库构建不包含它们。Mac 本地入口 `local_stub.py`、`test_local_stub.py` 也在第 1 项目录中，删除目录即可移除。

## 当前验证记录

已在当前 Mac 本地执行 Python 入口与 5 项 unittest，全部通过，其中包含 11 个 JSON 场景、三种查询 mask、反向源 PE、多实例、合并 layer 0、topology instance 隔离、错误参数、finalize 和缺失/损坏文件检查。JSON schema 校验也通过。当前环境没有 CANN、GoogleTest 和 JSON 第三方构建依赖，未编译或运行可选 C++ UT。Python 测试通过不能证明正式 C++ 或设备通信验收通过。
