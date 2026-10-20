# 追加需求与验收矩阵

1019 合并后的代码、复现命令与新验收结果见 [合并说明](../../tests/topology/README_1019_MERGE.md)。下文的历史验收记录保留其原日期，不代表新仓库设备验收。


## 8. 追加需求：平台、初始化配置与查询矩阵（2026-10-16）

本节补充原 RFC，保留前文首版设计和代码片段。涉及能力边界时以本节为准：950 的 SDMA 当前仅支持 CMO，不支持 PE-to-PE D2D；UDMA 需实际 QP0 路由。原第 4.1 节的生产规则据此细化，查询 ABI、位转换、物理发现与缓存流程继续沿用。

### 8.1 关系定义与成功前提

- D：目标内存段已导入且 `SdmaReaches(pe)` 为真。此内部函数也用于 MTE，不代表 950 支持 SDMA D2D。
- U：非 self PE 存在 transport manager，且 `GetQpNetLayer(UDMA, pe, 0, layer)` 成功。仅使能 UDMA 不足以满足 U。
- scaleup 为机内或超节点内可直访关系；scaleout 为仅 RDMA 连通的跨机/跨超关系。同一超节点跨机仍可能是 scaleup；同机跨直访组也可能 D=0。
- 以下成功结果均要求整个实例及对应 backend/设备资源初始化成功。D=0 且没有 RDMA 时，现有可达性检查返回 -100；必须记录初始化失败，不能打印为一次成功 query 的零引擎结果。
- 公开位 MTE/SDMA/ROCE/UDMA=1/2/4/8；内部 ROCE/SDMA=2/4，继续逐位转换。

### 8.2 非 self 引擎矩阵

A2 与 A3 分别执行以下每一行，单机和跨机均按实际 D 判定。

| 平台 | 初始化配置 | D=1 的 E | D=0 |
| --- | --- | --- | --- |
| A2/A3 | MTE 0x1 | 0x1 | 初始化可达性失败 -100 |
| A2/A3 | MTE+RDMA 0x5 | 0x5 | 0x4 |
| A2/A3 | MTE+SDMA 0x3 | 0x3 | 初始化可达性失败 -100 |
| A2/A3 | MTE+SDMA+RDMA 0x7 | 0x7 | 0x4 |

| 平台 | 初始化配置 | D=1,U=1 的 E | D=1,U=0 的 E | D=0 |
| --- | --- | --- | --- | --- |
| 950 | MTE 0x1 | 0x1 | 0x1 | 初始化可达性失败 -100 |
| 950 | MTE+RDMA 0x5 | 0x5 | 0x5 | 0x4 |
| 950 | MTE+UDMA 0x9 | 0x9 | 0x1 | 初始化可达性失败 -100 |
| 950 | MTE+UDMA+RDMA 0xd | 0xd | 0x5 | 0x4 |

D=0 时即便描述中有 UDMA 路由也不置 UDMA 位。RDMA 沿已有成功初始化后的使能规则置位，包含 self，不新增 self 排除政策。

| 边界配置 | 预期 |
| --- | --- |
| 未显式包含 MTE：0x2/0x4/0x6/0x8/0xc | create_entity 自动加入 MTE；D=1 时分别可得 0x3/0x5/0x7/0x9/0xd，仍受平台、路由和下述 CMO 约束 |
| 950 CMO：0x3/0x7/0xb/0xf | D=1,U=1 的 D2D E 分别为 0x1/0x5/0x9/0xd；U=0 时分别为 0x1/0x5/0x1/0x5；D=0 只有配置 RDMA 的成功实例返回 0x4 |
| A2/A3 self，配置 0x1/0x5/0x3/0x7 | 本地内存正常可达时 E=0x1/0x5/0x3/0x7，LINKS 仍按平台返回不支持 |
| 950 self，配置 0x1/0x5/0x9/0xd | E=0x1/0x5/0x1/0x5；LINKS 固定为空；带 CMO 位不增加 SDMA D2D 位 |
| A2/A3 输入含 UDMA | 既有非 950 扩展库初始化返回 NOT_SUPPORTED；不构造成功查询结果 |
| 950 RDMA | 分别验收启用 RDMA 的 XSCALE、HNS_1825 构建及匹配环境；资源失败打印实际 init_status |
| 0x0 配置 | 测试配置转换仍包含基础 MTE；公开 init 对该输入的接受范围以原参数校验为准，不能用配置转换测试代替 init 验收 |

950 在能力生产层排除 SDMA D2D，保留原 SDMA 配置与 CMO 资源初始化。`quiet_transport_mask` 独立按 SDMA 配置保留 CMO 完成等待，不依赖 D2D 能力位。无新增公开 CMO 查询位，也不改变原 SDMA API。

### 8.3 query_mask 返回矩阵

E 为 8.2 的结果，L 为物理描述匹配的升序去重数组；以下表与 8.2 作笛卡尔组合。

| 平台 | query_mask | status | engine_mask | count / pointer |
| --- | --- | --- | --- | --- |
| A2/A3 | ENGINES 0x1 | 0 | E | 0 / nullptr |
| A2/A3 | LINKS 0x2 或合并 0x3 | -100 | 错误不消费 | 错误不消费 |
| 950 | ENGINES 0x1 | 0 | E | 0 / nullptr |
| 950 | LINKS 0x2 | 0 | 0 | L.size() / L |
| 950 | 合并 0x3 | 0 | E | L.size() / L |
| 各平台 | 0 或含未定义位（含 0x4、0x5 等） | -2 | 错误不消费 | 错误不消费 |

空 L 使用 0/nullptr。合法参数未初始化=-5；空结果地址=-1；PE 越界、长度不足、ABI 不匹配=-2。多个错误同时存在时沿原参数校验顺序；状态验收每次只破坏一个条件。初始化状态与查询状态分开记录。

### 8.4 部署、物理输入与验收步骤

| 维度 | 必须覆盖的条件 |
| --- | --- |
| A2/910B 单机 | 相同 server，logicDeviceId/8 同组与跨组；结合内存导入核验 D |
| A3/Ascend910_93、950 单机 | 同 server 的身份判断与实际内存导入 |
| 各平台跨机 | 相同有效 superPodId 的直访域、不同或无效 superPodId 的非直访域；分别核验真实 D，不能用跨机直接等同 scaleout |
| LINKS 集合 | {}、{0}、{1}、{0,1}、{3}、{0,3}、{1,3}、{0,1,3} |
| 物理归并 | 单侧/双侧 IO DIE、多 plane、单端口/聚合多端口；同层只出现一次 |
| layer 3 | 两端同一非空全互通 CLOS RoCE 网络实例、相同非空 plane、IPV4/IPV6 地址族匹配；不同实例/plane/地址族及空 plane 的负例 |
| 配置独立性 | 固定 PE 绑定、成员和物理输入，改变引擎、1/2/4 QP、选层及 multi-link，L 保持一致 |
| QP 与物理层 | 物理 {0,1,3}、QP 仅配置 layer 0，LINKS 仍是 {0,1,3}；get_qp_net_layer 返回实际 QP 层；原 setter 的 layer 3 仍不支持 |

每种初始化组合均执行：记录平台/CANN/HDK/backend/设备与域身份 → 初始化并记录 init_status → 仅成功实例逐 PE 查询 1/2/3，并独立测试 0/4 等非法 mask → 比对 E、L 和空指针约定 → finalize。失败的初始化不调用 query 冒充有效实例。对于 A2/A3 的 UDMA 与 D=0 无 RDMA 的初始化失败，需要在设备环境单独验收。

Driver 输入取匹配产品和 Driver 的真实 topo_file_path。补充需求提供的 Driver 核对提交为 `9b9ad87cb8099fd741c93c93419a904ff05a60ce`，目录 `scripts/package/custom/driver/common/topo`，包含 atlas_350_1..3.json、atlas_850_1..4.json、atlas_950_1..2.json；SHMEM 的 mainboard_id 映射与产品策略读取 driver/topo/950/atlas_*.json。产品 JSON 索引不代表该型号已实机验证。

### 8.5 打印与本地回归

原 topology_query 模式保留，新增第五个位置参数 links=2 启用矩阵输出（第六个 engine_mask 仍使用十进制，例如 13 表示 0xd）。每个进程运行相同配置，每次初始化选择一组配置。示例：

```bash
./build/bin/topology_query 0 2 0 tcp://HOST:8899 2 13
./build/bin/topology_query 1 2 1 tcp://HOST:8899 2 13
```

以下是 950 D=1,U=1、L={0,1,3} 的预期格式，非实测日志：

```text
config=0xd local_pe=0 init_status=0
pe=1 query=0x1 status=0 engine_mask=0xd net_layer_count=0 net_layers=[]
pe=1 query=0x2 status=0 engine_mask=0x0 net_layer_count=3 net_layers=[0,1,3]
pe=1 query=0x3 status=0 engine_mask=0xd net_layer_count=3 net_layers=[0,1,3]
pe=0 query=0x3 status=0 engine_mask=0x5 net_layer_count=0 net_layers=[]
pe=1 query=0x0 status=-2
pe=1 query=0x4 status=-2
```

A2/A3 D=1、config=0x7 时 ENGINES=0x7；LINKS/合并 status=-100。该配置 D=0 成功实例 ENGINES=0x4。950 config=0x1/0x5/0x9/0xd、D=1,U=1 时合并查询 E=0x1/0x5/0x9/0xd，固定物理输入的 L 都为 {0,1,3}。

CPU 回归从生产源码提取 create_entity 的配置转换和 CanReachDataOperators 函数体，替代内存可达性及 QP0 getter，并接入原查询函数体；覆盖配置、D、self、manager/route 和 query_mask、八种缓存层集合。物理匹配 UT 另行从实际 endpoint/edge 输入覆盖八种集合和归并。它们验证控制流与返回合同，不替代完整 CANN 构建、初始化失败路径、真实设备身份与链路/CMO 数据面验收。旧 JSON stub 的 internal_transport_masks 是注入缓存，不应作为初始化配置或 950 CMO 能力生产证据。


## 本次实现改动与验证

- `mem_entity_default.cpp`：原函数内加 950 SDMA D2D 平台边界，UDMA 加 manager/QP0 路由条件。保留 MTE 直访、ROCE 使能及 self 的原规则。
- `shmem_init_backend.cpp`：950 CMO 的 quiet 掩码按原 SDMA 配置独立保留。未调整 public query 的转换和缓存实现。
- `shmem_host_init.h`：补充能力语义注释；结构体、公开位和签名不变。
- `examples/topology_query/main.cpp`：新增 links=2 矩阵打印模式及错误参数打印，保留原单次查询模式；原 SDMA 样例共享 helper 的 950 预期排除 SDMA，初始化配置保持。
- `engine_matrix_test.cpp`、`run_host_tests.py`：提取实际生产函数体，覆盖 384 个配置/可达性/self/manager/route 条件（非 950 128、950 256），对成功查询 fixture 继续组合八种缓存 L 和三种 mask；单独检查 CMO 完成掩码。
- `topology_layers_test.cpp`：新增八种物理 L、双 plane、多端口和单侧新增 plane 的归并测试；`topology_query_test.cpp` 增加已知/未知查询位混合的负例。
- 根目录 RFC.md 仅在文末追加第 8 节；现有实现、测试和示例保留。没有修改 shmem、shmem-publish，没有执行 reset、删除旧文件或提交。

2026-10-16 本地验证结果：两种平台宏分支各 53 个 C++ 测试通过（ASan/UBSan）；Python wrapper 4 个测试、原 JSON stub 5 个测试通过；三个示例分别在两种平台分支用真实 public headers 完成语法检查（仅替代外部 ACL/runtime 和 utility 声明）；`git diff --check` 通过。

这些结果不是 NPU 日志。A2/A3/950 完整 CANN 构建、单机/跨机真实 D/U、RDMA 两种 backend、UDMA 非支持平台的初始化失败、D=0 无 RDMA 的初始化失败，以及 CMO/quiet 数据面回归仍需在相应设备上按上表验收。原始文件临时快照位于 `/tmp/shmem-topology-before-20261016/`，其中 RFC.md 是追加前的完整原文。
