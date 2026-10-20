# 950 biaoka / UBX 独立验收

本目录已迁移到 shmem1019，采用 1019 已有 UDMA 逻辑并补齐 RFC 增量；合并取舍见 [1019 合并说明](../README_1019_MERGE.md)。results 为在新仓库重新运行的结果。

本目录独立重跑 RFC 的 950 本机验收，保留原实现、原始输入、此前九份拓扑测试和结果。不修改生产代码。

## 输入

- biaoka：`../cpp_json_stub/root_info-biaoka.json`，保留原始地址、plane、网络实例；其引用的物理文件在本机解析为 `../cpp_json_stub/atlas_350_3.json`。
- UBX：`../cpp_json_stub/root_info-ubx.json` 只有物理边。沿用用户批准的模拟 RootInfo，地址、plane、网络实例显式生成在 results 中。原物理边不变。
- 输入及 RootInfo 的 SHA256、生产代码指纹和命令回执保存在 results。两个输入均按 ACLSHMEM_SOC_950 编译，不能按 Atlas 文件名推断芯片。

## 执行

仓库根目录执行：

```sh
python3 -B tests/topology/950_biaoka_ubx/run.py
```

可追加 `--json /path/to/json --gtest /path/to/googletest`。默认依赖目录与通用验收脚本一致；未加 `--resume` 时重新执行全部命令。

## 覆盖及实现链路

每份输入覆盖公开引擎配置 1..15，D1/U1、D1/U0、manager 缺失、D0；包含 MTE+SDMA、MTE+ROCE、MTE+SDMA+ROCE、UDMA 和 CMO 组合、自动补 MTE、self/remote、三个 query_mask。每个源 PE 另跑四种基础配置，形成完整 PE×PE 矩阵。QP 数 1/2/4，各选层策略、多链路开关单独执行。另测资源失败、远端失败汇合、bootstrap 失败，以及成功阶段的十种参数和生命周期校验。

```text
原 RootInfo / 原物理边 + UBX 模拟 RootInfo
 → 实际 C++ ParseRootInfo / ParseTopoInfo / BuildLocalEndpoints
 → bootstrap 回放 → TopologyContext::Initialize
 → 生产 create_entity 配置转换、LoadExtendLibrary 函数体
 → CanReachDataOperators（D、manager、基础 QP0 为桩条件）
 → reach_info_init 能力装配 → topo_list
 → 生产 validate_peer_reachability（失败不发布查询实例）
 → build_topology_state / GetReachableNetLayers
 → aclshmemx_query_topology → backend::query_topology
 → C++断言 + 独立返回公式/物理边核验 → Excel、CSV、HTML
```

测试入口复用 `../run_acceptance.py --950-biaoka-ubx`，C++ 入口为 `../cpp_json_stub/run.py` 和 `main.cpp`；报告由 `../report_acceptance.py` 生成。QP 策略使用实际 C++ GetEidRoutes 和生产 QP 层 getter 函数体，但不创建硬件 QP。

Host UT 两种宏分支同时回归八种 LINKS 集合、初始化配置与设备身份矩阵、QP setter/getter 合同；保留旧 C++ fixture 及 Python 回归。单次查询记录包括 engine_mask、net_layers、query_mask、源/目标 PE、初始化配置及桩条件；初始化失败单独记录，不伪造成功查询。

## 结果与边界

- [验收统计与链路](results/README.md)
- [完整 Excel](results/950-biaoka-UBX验收结果.xlsx)
- [交互式 PE 返回矩阵](results/report.html)
- [完整 CSV](results/results.csv)
- [JSON 矩阵](results/summary.json)
- [输入和源码指纹](results/manifest.json)
- [命令回执](results/run-receipts.json)

950 的 SDMA 仅用于 CMO，engine_mask 不包含 SDMA D2D 位。D=1 且 U=1 时，非 self 的配置 3/5/7 分别返回 1/5/5；配置 9/13 返回 9/13。self 不包含 UDMA；D=0 且启用 ROCE 返回 4，否则在初始化门禁失败。LINKS 独立于引擎与 QP 策略。

这是本机 CPU 控制流验收。biaoka 原 RootInfo 的真实性也未在设备复核；两份输入的 D、基础 UDMA 路由和动态加载是桩。真实 CANN 构建、NPU、实际内存导入、硬件 QP、RDMA backend、CMO/quiet、完整 init 回滚仍为 **NOT_RUN**，不计为设备验收通过。
