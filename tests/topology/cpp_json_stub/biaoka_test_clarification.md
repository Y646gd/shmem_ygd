# 昇腾 950 标卡拓扑 C++ 本地打桩测试澄清

日期：2026-10-16。代码位置：`shmem-rfc/tests/topology/cpp_json_stub/`。

本文依据用户提供的 Mac 运行输出整理，说明测试输入、实际结果、字段含义、C++ 调用链以及结论边界。本次整理没有重新执行设备测试。

## 1. 本次验证的对象和结论

本次在 Mac 上通过 JSON 和底层桩，运行当前工作区的 C++ 拓扑发现、解析、物理匹配、缓存构建以及 `aclshmemx_query_topology()` 核心查询代码。

结果如下：

- 从 `root_info-biaoka.json` 自动识别 16 个设备，设备 ID 为 0–15；无需预先填写设备列表。
- 源为 PE0/设备0。对全部 16 个目标分别执行 ENGINES、LINKS、ENGINES | LINKS，共 48 次正常查询，status 均为 0。
- 组合查询时，设备0到自身的 LINKS 为 `[]`，到设备1–3为 `[0,3]`，到设备4–15为 `[3]`。
- ENGINES 输入默认模拟为 MTE，因此请求 ENGINES 时输出 `engine_mask=1`。
- 程序正常结束，输出 PASS；运行输出没有 ASan/UBSan 报错。

**本次证明的是：给定这份 JSON 输入，当前 C++ 核心路径可以执行，并产生上述结果。直接读取模式没有填写人工预期数组，因此尚未自动验证结果是否符合真实标卡物理组网预期，也没有验证真实设备通信。**

## 2. 输入文件和执行方式

从仓库根目录执行：

```bash
python3 -B tests/topology/cpp_json_stub/run.py
```

输入文件：

| 输入 | 内容与来源 |
| --- | --- |
| [root_info-biaoka.json](root_info-biaoka.json) | 用户提供的 RootInfo；包含 `rank_list`、设备 ID、local ID、网络实例、地址、端口与 plane |
| [atlas_350_3.json](atlas_350_3.json) | RootInfo 引用的物理拓扑文件；从本地 `driver/scripts/package/custom/driver/common/topo/atlas_350_3.json` 原样复制 |
| 默认源 PE | `0`；可由 `--source-pe` 调整 |
| 默认内部 transport 位 | `1`，即模拟 MTE；可由 `--transport-mask` 调整 |

RootInfo 原来的 `topo_file_path` 是服务器路径 `/usr/local/Ascend/driver/topo/950/atlas_350_3.json`。该文件在 Mac 上不存在时，测试查找 RootInfo 同目录的同名文件，并将临时 RootInfo 中的引用重定位为实际绝对路径。不会改写用户提供的原 RootInfo 文件，也不会生成虚构的物理边。

设备按 `rank_list` 的记录顺序映射为 PE。当前输入恰好是 PE0→device0、PE1→device1……，不代表 PE 编号在其他输入中必须等于设备 ID。

## 3. 实际输出结果

以下汇总 `query_mask=3` 的组合查询。每行 status 均为 0。

| 源设备 / PE | 目标设备 / PE | engine_mask | net_layer_count | net_layers |
| --- | --- | --- | --- | --- |
| 0 / 0 | 0 / 0 | 1 | 0 | `[]` |
| 0 / 0 | 1 / 1 | 1 | 2 | `[0,3]` |
| 0 / 0 | 2 / 2 | 1 | 2 | `[0,3]` |
| 0 / 0 | 3 / 3 | 1 | 2 | `[0,3]` |
| 0 / 0 | 4 / 4 | 1 | 1 | `[3]` |
| 0 / 0 | 5 / 5 | 1 | 1 | `[3]` |
| 0 / 0 | 6 / 6 | 1 | 1 | `[3]` |
| 0 / 0 | 7 / 7 | 1 | 1 | `[3]` |
| 0 / 0 | 8 / 8 | 1 | 1 | `[3]` |
| 0 / 0 | 9 / 9 | 1 | 1 | `[3]` |
| 0 / 0 | 10 / 10 | 1 | 1 | `[3]` |
| 0 / 0 | 11 / 11 | 1 | 1 | `[3]` |
| 0 / 0 | 12 / 12 | 1 | 1 | `[3]` |
| 0 / 0 | 13 / 13 | 1 | 1 | `[3]` |
| 0 / 0 | 14 / 14 | 1 | 1 | `[3]` |
| 0 / 0 | 15 / 15 | 1 | 1 | `[3]` |

当前只查询源 PE0 这一行关系，没有运行所有源 PE 的 16×16 全矩阵查询。

### 3.1 为什么每个目标打印三行

测试针对每个目标依次调用三个 query mask：

| query_mask | 查询内容 | 示例：PE0→PE1 |
| --- | --- | --- |
| 1 | ENGINES | `engine_mask=1, net_layer_count=0, net_layers=[]` |
| 2 | LINKS | `engine_mask=0, net_layer_count=2, net_layers=[0,3]` |
| 3 | ENGINES \| LINKS | `engine_mask=1, net_layer_count=2, net_layers=[0,3]` |

mask=1 时的空层数组表示没有请求 LINKS，不能据此判断物理不连通；mask=2 时 engine_mask=0 表示没有请求 ENGINES，不能据此判断实例没有使能引擎。

### 3.2 一条输出的含义

```json
{
  "case": "root_info-biaoka",
  "mode": "production_cpp",
  "source_pe": 0,
  "source_device_id": 0,
  "target_pe": 1,
  "target_device_id": 1,
  "query_mask": 3,
  "status": 0,
  "engine_mask": 1,
  "net_layer_count": 2,
  "net_layers": [0, 3],
  "engine_expectation_checked": false,
  "links_expectation_checked": false
}
```

| 字段 | 解释 |
| --- | --- |
| case | 输入场景名，来自 RootInfo 文件名 |
| mode | `production_cpp` 表示执行当前源码中的 C++ 查询函数体；不是此前的 Python 行为模型 |
| source_pe / target_pe | 当前测试实例中的源、目标 PE 编号 |
| source_device_id / target_device_id | 从 RootInfo 读取的设备 ID |
| query_mask | 请求的信息集合 |
| status | 真实 C++ 公开查询返回码；0 为成功 |
| engine_mask | 公开引擎位集合，本次为模拟输入经真实 C++ 位转换产生的结果 |
| net_layer_count | 返回的网络层种类数；不是端口数、plane 数或 QP 数 |
| net_layers | 真实 C++ 物理匹配产生的有序去重层集合 |
| engine_expectation_checked / links_expectation_checked | 是否与人工填写的预期数组比较；本次均为 false |

### 3.3 LINKS 的解释

- `[]`：本次源查询自身，按接口合同返回空集合。
- `[0,3]`：当前描述与算法匹配出了有效 layer 0 关系，以及显式 RoCE layer 3 网络实例关系。
- `[3]`：当前描述与算法只匹配出本期输出范围中的 layer 3。
- 未返回 layer 1：说明当前输入没有被该算法匹配为有效 layer 1，不等于已证明真实设备不存在或无法使用该层。

RoCE layer 3 按两端的层值、CLOS 网络类型、有效且相同的网络实例、非空且相同的 plane，以及兼容 IP 地址族判断。没有实际发包探测网络、检查实时端口状态或验证 RoCE fabric 全互通性。

### 3.4 ENGINES 的解释

RootInfo 描述物理网络，不包含当前 SHMEM 实例的引擎使能和资源建立结果，因此不能从 JSON 的端口/IP 自动推导实际引擎可用性。

本测试将内部 `topo_list` 默认注入为 MTE=1，再执行真实 backend 转换。两套位定义为：

| 引擎 | 内部 transport 位 | 公开 engine 位 |
| --- | --- | --- |
| MTE | 1 | 1 |
| ROCE | 2 | 4 |
| SDMA | 4 | 2 |
| UDMA | 8 | 8 |

因此 `engine_mask=1` 是本次模拟配置的结果，不是 Mac 探测到昇腾 MTE 硬件的结果。LINKS 包含 3 而 engine_mask 只有 MTE 并不矛盾：物理网络关系和当前使能引擎是不同的信息。

## 4. 完整执行与代码调用链

### 4.1 构建阶段：Python 只调度 C++ 编译

入口：[run.py](run.py)。

1. 创建临时构建目录和依赖替身头文件。
2. 定义 `ACLSHMEM_SOC_950`，启用 950 编译分支。
3. 定义 `ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO`，将 RootInfo 读取位置指向临时文件。
4. 原样提取当前生产源码中的 ABI 断言、公开 query 和 backend build/query 函数体，生成临时 `queries.cpp`。
5. 与真实 `topo_reader.cpp`、`topology_context.cpp` 和测试 `main.cpp` 一起编译。
6. 启用 ASan/UBSan，启动生成的 C++ 程序；结束后删除临时构建目录。

这不是链接完整 SHMEM 动态库。两个初始化大编译单元依赖大量 CANN/驱动接口，因此只原样提取本次需要验证的函数体编译；提取代码使用 `#line` 指回生产源码。每次执行都会读取当前源码，不维护一份手写的 C++ 查询算法副本。

### 4.2 C++ main → RunCase：准备文件

入口：[main.cpp](main.cpp) 的 `main()`，读取场景配置并调用 `RunCase()`。

直接读取模式由 runner 生成一条配置，包含 RootInfo 路径、源 PE 和模拟 transport 位，不包含设备列表和人工预期数组。

`RunCase()`：

```cpp
auto input = Read(rootPath);
// 定位实际拓扑文件，重定位临时 RootInfo 的 topo_file_path
std::ofstream output(TopoReader::ROOTINFO_PATH);
output << input.dump(2);
```

遍历 `input["rank_list"]` 读取设备 ID，检查重复 ID 和 rank_count，然后打印 `[DEVICE]` 映射。

### 4.3 PreparePeers：准备其他 PE 将发送的描述

`RunCase()` 调用 `PreparePeers(devices, source)`。对每个设备执行真实 C++：

```text
TopoReader::ParseRootInfo()
TopoReader::ParseTopoInfo()
TopoReader::GetLocalId()
TopoReader::GetEidCount()
TopologyContext::BuildLocalEndpoints()
nlohmann::json::to_msgpack()
```

结果是各 PE 的 local ID、地址数量和 MessagePack endpoint payload，用于替代真实 bootstrap 中其他 PE 的发送内容。

这一步不会生成 LINKS 答案，也不会读取 expected_layers 来拼接 payload。

### 4.4 真实 TopologyContext::Initialize：发现与交换

`RunCase()` 调用：

```cpp
context->Initialize(devices[source], source, devices.size(), &fixture.boot);
```

真实实现位于 [topology_context.cpp](../../../src/host/transport/topo/topology_context.cpp)。执行顺序为：

```text
设备 ID 转换（runtime 桩）
  ↓
ParseRootInfo()
  ├─ LoadRootInfoJson()：真实 ifstream 读取临时 JSON
  └─ ParseRootInfoJson()：真实字段解析、地址转换
  ↓
ParseTopoInfo()：真实读取 topo_file_path 对应文件，解析 Mesh/Clos 记录
  ↓
GetLocalId() / GetEidCount()
  ↓
BuildLocalEndpoints() → MessagePack 编码
  ↓
bootstrap allgather 桩回放
  ↓
真实 MessagePack 解码，写入 context 成员
```

解析与匹配实现位于 [topo_reader.cpp](../../../src/host/transport/topo/topo_reader.cpp)。

### 4.5 bootstrap 函数指针如何跳到桩

测试设置：

```cpp
fixture.boot.bootstrap_state = &replay;
fixture.boot.allgather = Replay::Gather;
```

所以真实初始化代码中的：

```cpp
bootstrap->allgather(send, receive, bytes, bootstrap);
```

会跳转到测试的 `Replay::Gather()`。初始化阶段按以下顺序回放：

| 调用阶段 | 数据 |
| --- | --- |
| 1 | 本地准备状态一致性 |
| 2 | 各 PE EID 数量 |
| 3 | 各 PE local ID |
| 4 | 各 PE MessagePack 长度 |
| 5 | 缓冲区准备状态一致性 |
| 6 | 各 PE MessagePack endpoint 内容 |
| 7 | 解码状态一致性 |

payload 来自第 4.3 节的真实 C++ 解析结果；桩同时核对本地发送内容。状态一致性只模拟对端成功，不代表验证了真实多进程通信。

`Initialize()` 返回后，context 保留 `root_info`、`topo_info`、`rank_to_local_id` 和 `rank_to_sync_endpoints`。

### 4.6 绑定测试实例，保留发现结果

使用最小 backend fixture 登记实例，随后：

```cpp
fixture.backend.entity_map_.at(fixture.instance.id)->topology_context = std::move(context);
fixture.state.topo_list[peer] = static_cast<aclshmem_transport_t>(masks[peer]);
```

context 所有权保存在实例对象里，topo_list 保存模拟引擎输入。同时设置 `init_manager`、`g_instance_ctx` 和对应全局状态，使公开 query 能找到当前实例。

这里替代了完整初始化中的资源绑定和硬件建链，不会调用真实 `reserve_heap()` 或分配设备 heap。

### 4.7 真实 build_topology_state：计算并保存 LINKS

测试调用：

```cpp
fixture.backend.build_topology_state(fixture.instance.id);
```

执行 [shmem_init_backend.cpp](../../../src/host/init/backends/shmem_init_backend.cpp) 中原样提取的方法：

```text
按 instance_id 找到 entity_member
  ↓
读取 elem->topology_context
  ↓
构造 TopoQuerier
  ↓
对每个目标调用 GetReachableNetLayers()
  ├─ Mesh 直连边、端口、网络实例匹配
  ├─ Clos 层、附件、topology instance、plane 匹配
  ├─ EffectiveRouteNetLayer() 归一化
  ├─ HasRoceLayerRelation() 判断 layer 3
  └─ 排序、去重
  ↓
保存到 elem->topology_net_layers_by_pe
  ↓
第 8 次 allgather：构建状态一致性
```

因此网络层答案由真实生产算法计算，不由测试提前填写。

### 4.8 真实 public query → backend query：返回结果

缓存就绪后，测试将实例标记为初始化完成，针对每个目标调用 mask=1/2/3：

```cpp
aclshmemx_topology_t output = ACLSHMEMX_TOPOLOGY_INITIALIZER;
int status = aclshmemx_query_topology(peer, mask, &output);
```

公开函数体来自 [shmem_init.cpp](../../../src/host/init/shmem_init.cpp)：

```text
检查指针、mask、结构体大小、ABI
  ↓
加全局上下文锁，检查初始化状态和 PE 范围
  ↓
init_manager->query_topology(g_instance_ctx->id, pe, mask, output)
  ↓
backend 加实例 map 锁，定位 entity_member
  ├─ ENGINES：读取 topo_list，转换公开位
  └─ LINKS：返回 topology_net_layers_by_pe[pe] 的长度和数组指针
  ↓
返回 C++ 测试，读取输出、检查合同并打印 JSON
```

数据实际保留位置：

```text
backend fixture
  └─ entity_map_[instance_id]
       ├─ entity_host_state->topo_list          模拟引擎输入
       ├─ topology_context                     真实发现/解析结果
       └─ topology_net_layers_by_pe             真实物理匹配结果
```

## 5. 真代码和桩的边界

| 环节 | 本次实际执行 |
| --- | --- |
| RootInfo / TopoInfo 解析、地址转换 | 直接编译生产 C++ 文件 |
| endpoint 生成、MessagePack 编解码、Initialize 控制流 | 直接编译生产 C++ 文件 |
| GetReachableNetLayers 和物理匹配 helpers | 直接编译生产 C++ 文件 |
| ABI 静态断言、public query、backend build/query | 从当前生产源码原样提取并编译 |
| 设备 ID 查询 | 桩返回测试编号 |
| RootInfo 来源 | 临时 JSON 文件，编译宏重定向路径 |
| 多 PE allgather | 单进程回放各 PE 的 JSON 描述 |
| backend 容器、bind/release | 最小 fixture；不是原 backend 完整对象布局 |
| 引擎可达性发现 | 不执行；模拟 topo_list，执行真实公开位转换 |
| 日志、文件辅助接口 | 本机替身实现；未验证原文件辅助模块的全部行为 |
| 完整 init/finalize、HYBM、UDMA manager、QP、设备内存、真实流量 | 本次不覆盖 |

此前的 Python 行为模型仍保留，但本命令没有调用 Python 模型来计算 ENGINES 或 LINKS。

## 6. CHECK 和 PASS 的准确含义

原始输出：

```text
[CHECK] root_info-biaoka: engines expected=0, links expected=0 (0 = observation only)
PASS: 1 C++ JSON cases (real public query, ASan+UBSan)
```

`expected=0` 是“没有启用人工预期值比较”的标志，不是“预期 engine 或层集合为零”。对应 JSON 的两个 `expectation_checked` 字段均为 false。

PASS 表示本次输入完成执行、正常查询成功，并通过测试代码中的基本合同检查，包括未初始化、空输出、非法 PE、零 mask、短结构体长度、reserved 保留和空层数组为空指针等。ASan/UBSan 在此次运行中未报告错误。

PASS 不表示以下事项已经验收：

- 这份标卡的实际物理预期一定是当前输出。
- RoCE fabric 已经验证全互通，或 live 链路健康。
- 完整 SHMEM 初始化、C++ 所有改动、全部 QP 路由分支已经通过测试。
- 实际通信引擎、双层数据搬运或性能已经得到验证。

要进一步作为预期值回归测试，可在套件中独立填写经确认的 `expected_engine_masks` 和 `expected_layers`，通过 `--fixtures` 运行。预期值应来自需求/物理组网确认，不能直接把本次输出复制成唯一正确性依据。

## 7. 复现及原始记录

```bash
# 本文对应的默认输入，源 PE0，模拟 MTE
python3 -B tests/topology/cpp_json_stub/run.py

# 改变源 PE
python3 -B tests/topology/cpp_json_stub/run.py --source-pe 1

# 显式指定 RootInfo 和物理拓扑文件
python3 -B tests/topology/cpp_json_stub/run.py \
  --root-info /absolute/path/rootinfo.json \
  --topology /absolute/path/topo.json

# 原有带人工预期值的两个基础回归场景
python3 -B tests/topology/cpp_json_stub/run.py \
  --fixtures tests/topology/cpp_json_stub/cases.json
```

本次用户粘贴的完整运行输出另存为 [biaoka_test_output.log](biaoka_test_output.log)。打桩接口、依赖和正式版本删除清单参见 [readme.md](readme.md)。
