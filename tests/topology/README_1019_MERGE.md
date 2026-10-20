# RFC 合并到 shmem1019

需求来源：`/Users/Admin/Projects/RFC.md`，含第 9～11 节补充。
目标基线：origin/master `a23c173`；来源为 shmem-rfc `155d161` 上的工作区增量。
原 shmem-rfc 和 RFC.md 均保留，不复制旧运行结果，不提交或推送代码。

## 实现取舍

| RFC 需求 | 1019 现状 | 本次处理 |
| --- | --- | --- |
| UDMA 非 self、D、使能、manager、QP0 判断 | 已实现 | 保留 1019 原逻辑 |
| 无 UDMA 路由允许其他后端工作 | 已实现 | 保留 NOT_SUPPORTED 回退和真实建链失败行为 |
| Mesh 跨 server、Clos 网络实例约束 | 已实现 | 保留原 TopoQuerier；共享 context 接入后仍在 UDMA 阶段交换 serverId |
| shared jetty、QP slot、运行配置冻结 | 已实现 | 保留 1019 的完整配置传递和 endpoint/channel 实现 |
| 全远端能力非零检查 | 本地检查后立即返回 | 按 RFC 第 10.1 节改为 setup_heap 后的全 PE 状态门禁，避免重复保留本地提前返回 |
| Host ENGINES/LINKS 查询、64 字节 ABI | 缺失 | 迁入公开头、public/backend query、生命周期说明 |
| 950 物理层 0/1/3 枚举及缓存 | 缺失 | 迁入物理匹配与实例缓存，不修改原 UDMA 候选选择规则 |
| 共用 TopologyContext | 缺失 | 迁入 builder；SHMEM 实例持有，UDMA 借用；独立 HYBM 可自建 |
| 950 SDMA 仅 CMO，不发布 D2D 位 | 缺失 | 迁入能力排除和独立 CMO quiet 掩码 |
| Python、示例、文档、打桩回归 | 查询功能缺失 | 迁入并保留 1019 原有接口和运行配置 |

context 发现仍为七次交换；UDMA 另外保留 serverId 获取/汇合/交换，不能将两者合称为七次完整初始化交换。

## 本机测试

在 shmem1019 根目录运行：

```sh
python3 -B tests/topology/run_acceptance.py --output tests/topology/results/merged-1019-all
python3 -B tests/topology/report_acceptance.py --output tests/topology/results/merged-1019-all
python3 -B tests/topology/950_biaoka_ubx/run.py
python3 -B tests/topology/run_udma_merge_tests.py
```

通用验收和独立 biaoka/UBX 验收均接受 `--json` 与 `--gtest` 指定本机依赖；UDMA 衔接测试接受 `--json`。
当前默认使用 `/tmp/shmem-rfc-json-v3.11.3` 和 `/tmp/shmem-gtest-complete/googletest-1.14.0`。

保留的测试：

- Host 宏分支测试：真实 parser/router/context、查询函数体、能力生成、身份矩阵、QP 合同；包含 1019 的跨 server/网络实例负例。
- C++ JSON runner：生产发现、能力、门禁、网络层缓存和 public query；包括九份拓扑与 biaoka/UBX 两套输入。
- 旧 raw transport 注入两例：只验证缓存/查询，不作为真实能力生成证据。
- 原 guarded JSON context 注入 UT、Python wrapper 与 Python JSON 回归。
- 新增 UDMA 衔接测试：提取生产 PrepareOpenDevice、EndpointQpSlot 和 context 安装函数体，连接真实 router/context；检查共享 context、serverId、无路由回退、endpoint 失败、shared jetty 多 QP 槽位、bootstrap 和注入所有权。

结果保存在各 output 目录，包含 manifest、命令回执、初始化/查询分开记录、JSON/CSV/HTML/Excel。
生成物在 tests/topology/.gitignore 中排除，源码和原始 JSON 输入保留。

## 验证边界

本机 C++ runner 使用 ASan/UBSan。D、基础 QP 路由、bootstrap、动态加载及 endpoint 创建存在明确测试桩；UBX RootInfo 仍为模拟。
不宣称真实 CANN 构建、符号导出、pybind 链接、完整 OpenDevice、硬件 QP/channel、shared jetty 数据面、CMO/quiet、真实初始化回滚已经通过。
原 guarded JSON UT 已迁入 CANN 测试树；本机额外提取安装函数体验证其合同，不能代替完整 CANN 单测二进制。

## 已完成的验证记录

- Host 两种编译分支各 60 项通过（ASan/UBSan）。
- biaoka/UBX：302 场景，282 个成功阶段、20 个预期失败阶段，13,536 条查询全部核验通过；参数校验 2,820 条、QP 观察 768 条。
- 新增 UDMA/context 衔接检查：12 + 4 项通过。
- 与 a23c173 比对：44 个 UDMA manager 方法、4 个路由入口函数体保持一致；变动集中于 context 发现与接入。
- 九份拓扑及 biaoka/UBX 原始输入共 12 份内容逐字节一致。
- 记录：[UDMA 衔接与源码指纹](results/udma-merge-validation.json)、[biaoka/UBX 新报告](950_biaoka_ubx/results/README.md)。

九份拓扑完整验收已通过，报告与命令回执核验完成：

| 分支 | 场景 | 成功阶段 | 预期失败阶段 | 查询记录 |
| --- | --- | --- | --- | --- |
| A2 | 985 | 787 | 198 | 74,220 |
| A3 | 985 | 787 | 198 | 74,220 |
| 950 | 1,471 | 1,381 | 90 | 108,276 |

共 3,441 场景、256,716 条查询记录；A2/A3 的 LINKS 返回不支持属于预期结果。另有 29,550 条参数/生命周期校验、4,128 条 QP 路由观察，九份输入的完整 PE 矩阵通过独立物理边核验。

[九份拓扑报告](results/merged-1019-all/README.md) · [交互报告](results/merged-1019-all/report.html) · [Excel](results/merged-1019-all/九份拓扑完整本机验收结果.xlsx)
