# Mac 本地 C++ 拓扑 JSON 桩（独立于 Python 行为模型）

本套测试验证当前工作区的真实 C++ `aclshmemx_query_topology()` 及其数据生产路径，JSON 只替代底层硬件输入。此前 `../json_stub/` 中的 Python 模型及测试全部保留。

## 在 Mac 运行

需要 Xcode Command Line Tools 的 `clang++`、Python 3（只负责构建）、nlohmann/json 头文件。无需 CANN、昇腾硬件、GoogleTest、Python 第三方包。当前机器已有依赖位于 `/private/tmp/shmem-rfc-json-v3.11.3`，默认自动发现；不需要下载。

从仓库根目录执行，先只看基础场景：

```bash
python3 -B tests/topology/cpp_json_stub/run.py --case mixed_engines_and_layers
```

运行原来的两个预期值回归场景：

```bash
python3 -B tests/topology/cpp_json_stub/run.py --fixtures tests/topology/cpp_json_stub/cases.json
```

依赖不在默认位置时，显式指定包含 `single_include/nlohmann/json.hpp` 的源码目录：

```bash
python3 -B tests/topology/cpp_json_stub/run.py --json /path/to/nlohmann-json
```

自己的套件：

```bash
python3 -B tests/topology/cpp_json_stub/run.py \
  --fixtures /absolute/path/cases.json --case my_case
```

`run.py` 编译、启动 C++ 可执行程序并检查退出码；没有任何 Python 拓扑解析、匹配、引擎计算或查询实现。构建临时目录退出后自动删除。编译启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。

## 直接读取 root_info-biaoka.json，自动识别设备

现在不传参数默认读取本目录的 `root_info-biaoka.json`：

```bash
python3 -B tests/topology/cpp_json_stub/run.py
```

也可以指定其他 RootInfo，不需要提供 devices 列表或 expected 数组：

```bash
python3 -B tests/topology/cpp_json_stub/run.py --root-info /absolute/path/rootinfo.json
```

C++ 从 `rank_list` 按文件顺序取得 `device_id`，依次映射到 PE0、PE1……，打印 `[DEVICE]` 映射；检查重复 ID 和 `rank_count`。本文件自动识别 16 个设备 ID 0–15。这里读取的是设备编号，JSON 不含可查询的独立设备名称。

默认源为 PE0，可通过 `--source-pe 1` 切换。RootInfo 不包含当前 SHMEM 实例的引擎使能可达状态，不能由网络地址推导真实 ENGINES；默认对所有 PE 注入 MTE 内部位 1，可通过 `--transport-mask 0xf` 修改（内部位 MTE=1、ROCE=2、SDMA=4、UDMA=8）。

拓扑文件首先使用 RootInfo 的 `topo_file_path`；若服务器绝对路径在 Mac 上不存在，则在 RootInfo 同目录查找同名文件。已从仓内 `driver/scripts/package/custom/driver/common/topo/atlas_350_3.json` 原样复制为本目录的 `atlas_350_3.json`，用于本文件。可用 `--topology /absolute/path/topo.json` 显式指定。缺少真实拓扑文件会直接报错，不生成虚构边。

直接读取模式只打印真实 C++ 结果，并检查查询状态、参数和 ABI 基本合同；没有人为填写预期，因此输出中的 `engine_expectation_checked`、`links_expectation_checked` 为 false。此模式运行成功不等于已经证明物理预期正确。需要回归断言时，继续用 `--fixtures cases.json` 或 `--case mixed_engines_and_layers`，保持原来的 expected 检查。

例如本次实际运行（源设备 0，组合查询）：自身层为空，PE1–PE3 返回 `[0,3]`，PE4–PE15 返回 `[3]`；引擎位为默认模拟 MTE=1。层结果由当前 C++ 和输入描述产生，不是输入的预期值。

## 实际编译的代码与边界

直接编译原文件，不复制或改写算法：

- `src/host/transport/topo/topo_reader.cpp`：真实 RootInfo/TopoInfo 解析、地址转换、Mesh/Clos/RoCE 物理匹配、层归一化、去重。该文件中的路由方法也参与编译，但本用例不执行所有 QP 路由分支。
- `src/host/transport/topo/topology_context.cpp`：真实设备发现控制流程、endpoint 生成、MessagePack 编解码、bootstrap 交换次序与状态一致性。

为避开两个巨大初始化编译单元中无关的 CANN/驱动依赖，每次构建从当前源文件原样提取、编译：

- `shmem_init.cpp` 中冻结布局常量、ABI 静态断言及 `aclshmemx_query_topology()`。
- `shmem_init_backend.cpp` 中 `aclshmemi_init_backend::build_topology_state()` 和 `query_topology()`。

生成文件使用 `#line` 指回真实源码位置。这里不是链接完整 SHMEM 库，也没有编译整个 `shmem_init.cpp`；编译的是其原样函数体。改动上述生产函数后，下次运行会重新读取、编译，不维护第二份查询算法。源码提取失败或编译失败直接使测试失败。

替身边界：

| 替换项 | 桩的行为 |
| --- | --- |
| 设备 ID runtime 调用 | user/logical/physical ID 使用测试设备编号，返回成功 |
| RootInfo 文件位置 | 编译宏指定独立临时 JSON，不访问或覆盖 `/etc` |
| bootstrap allgather | 根据所有 PE 的 JSON，经真实 C++ parser/endpoint builder 构造远端 payload，再按真实交换阶段回放；检查本地发送内容 |
| backend 资源容器、bind/release | 最小 C++ fixture，保留查询需要的 state、锁、实例 map、context owner 和缓存；不创建真实 heap/entity |
| `topo_list` | 从 `internal_transport_masks` 注入，实际公开 engine 位转换使用真实 backend 方法 |
| FileUtil/日志 | 使用本机 filesystem；日志丢弃；不验证原 FileUtil 的全部安全检查行为 |
| 硬件 rootinfo fallback | 禁止，调用即返回失败，并检查没有发生 fallback |

不执行完整 `aclshmemx_init_attr()`、`create_entity()`、`reserve_heap()`、HYBM/QP 建链、UDMA manager ownership 集成、内存映射和数据搬运。它验证查询、真实发现/解析/匹配及缓存，而不是所有相对基线的修改或全部初始化生命周期。正式公开头文件、Host state、bootstrap handle 布局仍使用仓内原定义；backend fixture 不是原 backend 的完整布局。

## 真实执行链

```text
Python run.py：只编译和启动
  ↓
C++ main() / RunCase()
  ├─ 读 cases.json 与指定 RootInfo
  ├─ 只将 topo_file_path 重定位成绝对路径
  ├─ 写入临时 RootInfo JSON
  ├─ PreparePeers()
  │    └─ 对每个 PE 调真实 ParseRootInfo / ParseTopoInfo / BuildLocalEndpoints
  │         → 仅生成远端 bootstrap 将发送的数据，不生成预期层集合
  ├─ 真实 TopologyContext::Initialize()
  │    ├─ 设备 ID 桩
  │    ├─ 真实 ParseRootInfo()
  │    │    └─ 真实 LoadRootInfoJson() 读取临时 JSON
  │    │         └─ 真实 ParseRootInfoJson()
  │    ├─ 真实 ParseTopoInfo() 读取拓扑文件
  │    ├─ 真实 BuildLocalEndpoints() 和 MessagePack 编码
  │    └─ allgather 桩回放 → 真实 MessagePack 解码
  ├─ fixture 绑定实例，把发现完成的 context 交给实例 owner
  ├─ topo_list 注入模拟引擎位
  ├─ 真实 build_topology_state()
  │    └─ 真实 GetReachableNetLayers() → 实例缓存
  ├─ 标记测试实例已初始化
  └─ 真实 aclshmemx_query_topology()
       └─ 真实 backend::query_topology()
            ├─ topo_list → 公开 engine bits
            └─ 返回实例 LINKS 缓存指针
                 ↓
            C++ 断言 expected_*，打印实际结果
```

`expected_*` 只参与最后断言，绝不会用于创建拓扑、交换 payload、填充缓存或计算查询结果。查询失败、预期不符、sanitizer 报错均使命令非零退出。

## 先保留两个简单场景

`cases.json` 引用已有 JSON 数据文件，不修改原文件。

| 场景 | 引擎内部输入 | 公开 engine 输出（目标 PE0/1/2/3） | LINKS（目标 PE0/1/2/3） |
| --- | --- | --- | --- |
| mixed_engines_and_layers | `[0,15,2,4]` | `[0,15,4,2]` | `[] / [0,1,3] / [1] / [3]` |
| mte_only_links_unchanged | `[1,1,1,1]` | `[1,1,1,1]` | 同上 |

源均为 PE0。每个目标依次调用 query mask 1/2/3。输出中 `mode=production_cpp`，与旧 Python 模型输出区分。还检查未初始化、空输出、非法 PE、零 mask、短 ABI、reserved 保留和空集合空指针。

自定义文件格式同 `../json_stub/readme.md`，相对路径均以新套件文件所在目录为准。本 C++ runner 目前只接收能成功解析的场景（`parse_ok=true`）；旧套件的两个预期解析失败用例不在本套范围中，输入非法会作为失败直接退出。

## 本套新增与删除清单

新增目录 `tests/topology/cpp_json_stub/`：

- `run.py`：编译调度、头文件替身和生产函数提取。
- `main.cpp`：底层 runtime/bootstrap 桩、JSON fixture、真实 C++ 调用与断言。
- `cases.json`：两个预期值测试场景。
- `atlas_350_3.json`：从 driver 源码原样复制的物理拓扑文件。
- `root_info-biaoka.json`：用户提供的 RootInfo 输入，删除测试代码时先保留或备份这份用户数据。
- `readme.md`：本文。

生产源码只新增一处：`src/host/transport/topo/topo_reader.h` 中 `ROOTINFO_PATH` 周围的 `ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO` 条件分支，标记 `TEMP_TOPOLOGY_CPP_JSON_STUB`。宏只由本 runner 定义；正常构建仍是 `/etc/hccl_rootinfo.json`。

正式版本移除本套时：删除本目录，将上述条件分支恢复为单行原定义即可。不要删除此前的 Python 测试或其数据文件；本套引用了那些数据。

```bash
rg -n 'TEMP_TOPOLOGY_CPP_JSON_STUB|ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO' src tests/topology/cpp_json_stub
```

## 950 配置驱动的能力生成测试

原 internal_transport_mask(s) 模式保留，仅用于缓存位转换测试，不能用含 SDMA 的注入结果宣称 950 支持 SDMA D2D。

新增 fixture 字段 engine_config（公开配置位，当前仅 MTE/SDMA/ROCE）与 direct_reachable（每个 PE 的显式 bool 数组，self 必须为 true）；与 internal_transport_mask(s) 互斥。此模式从生产源原样提取 create_entity 的 options 配置转换、CanReachDataOperators，以及 reach_info_init 的 transport 位装配，再调用原查询接口。仅 SdmaReaches 使用指定的 D，QP getter 返回不支持（本模式不接受 UDMA）。不能由 LINKS 或 JSON 推断 D；不执行 backend/设备初始化。远端能力为零时拒绝构造成功查询 fixture，不把它伪装成成功 init。

示例配置：engine_config=3，direct_reachable=[true,true,...]；950 的 expected_engine_masks 应全为 1。engine_config=5/7 时应为 5。LINKS-only 的 engine_mask 仍为零。

本次两份输入的结果与可复现 fixture 见 ../results/950-20261016/950-capability-report.html 和 950-capability-cases.json。旧缓存注入模式的两项原始 fixture 回归继续通过。


[950 完整配置驱动测试链路与代码](README_950_TEST_CHAIN.md)：含生产函数体、完整 runner/main 源码、打桩边界、断言和 Excel 导出链。


## 2026-10-19 完整本机验收入口

新增 [README_ACCEPTANCE.md](../README_ACCEPTANCE.md) 与 `run_acceptance.py` / `report_acceptance.py`，保留本文历史说明。当前runner已支持 `--platform A2|A3|950`，配置模式接受1..15（含UDMA），显式direct_reachable、transport_manager、udma_qp_layers；可用route_qp_count/qp_policy/multi_link调用实际C++选路，构造getter所需的路由描述。新增resource_status、remote_init_status、gate_transport_failure和expected_init_status注入与核对初始化阶段失败。acceptance_mode记录初始化阶段、QP选路与错误参数，失败时不发布initialized。

正式初始化已接入validate_peer_reachability，不再只在桩中自行拒绝无引擎目标。宏分支、配置转换、扩展库加载、能力生成、门禁、query均执行生产函数体；动态加载、设备资源、段导入和bootstrap仍为桩。UBX仍为模拟RootInfo，完整设备验收NOT_RUN。
