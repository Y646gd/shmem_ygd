# RFC 全文 UT 与需求证明

审查实现请先看 [需求—实现代码—UT断言—实际结果](../../docs/api/rfc_implementation_ut.md)：逐章贴出正式实现、测试代码、本轮XML与原始返回值，不仅是打桩数量统计。

需求基准：[RFC.md](/Users/Admin/Projects/RFC.md)，以第 9～11 节补充明确的平台/CMO/UDMA和本机验收边界为准。

## 执行

在当前 shmem1019 根目录：

```sh
python3 -B tests/topology/run_rfc_ut.py \
  --json /tmp/shmem-rfc-json-v3.11.3 \
  --gtest /tmp/shmem-gtest-complete/googletest-1.14.0
```

输出默认为 [results/rfc-ut-20261020](results/rfc-ut-20261020/README.md)。用 `--output` 选择新目录；`--resume` 只在 RFC 及全部被测代码/输入指纹相同的条件下复用成功命令。报告生成器自身的修正允许不重新编译已通过的 UT，回执用 reused 字段明确标注。

报告包括：

- 按 RFC 各章/主要条款列出的需求、测试、结论和限制；需求索引为 [rfc_requirements.json](rfc_requirements.json)。
- Host A2/A3/950 分支、规划示例的 GoogleTest XML，含每条测试的结果。
- 命令、退出码、日志 SHA256、当前实现和 RFC 指纹、依赖与编译器信息。
- 九份原始拓扑的源 PE0/末源 PE四种配置和失败样本、biaoka/UBX全302例、旧 C++缓存注入两例。

本轮没有冒充重新执行全部源目标对。此前完整3,441场景/256,716查询为[历史验收](results/merged-1019-all/README.md)，新结果单独存放。

## 新增和加强的 UT

| 文件 | 被测对象与目的 |
| --- | --- |
| [rfc_requirements_test.cpp](rfc_requirements_test.cpp) | 实际 public/backend query、build、gate 函数体；全字段 ABI、扩展尾部、世界 PE、全部未知位、快照借用、多实例及门禁 |
| [topology_layers_test.cpp](../unittest/host/topo/topology_layers_test.cpp) | 直接编译真实物理匹配；补端口/fabric/层/未知域、聚合端口等价、layer2排除、多PE RoCE域 |
| [context_test.cpp](context_test.cpp) | 直接编译真实 Initialize；补运行时映射失败、无效参数、零EID数量、过大远端消息和解码状态汇合 |
| [engine_matrix_test.cpp](engine_matrix_test.cpp) | 提取真实配置转换、能力和扩展加载；新增独立写死的RFC self/远端期望表与资源失败测试 |
| [planner_test.cpp](planner_test.cpp) | 直接包含实际 VerifyTwoLayerPlan，资源查询是桩；提取Host/kernel真实分段算术验证覆盖和无重叠 |
| [run_udma_merge_tests.py](run_udma_merge_tests.py) | 提取实际 OpenDevice/PrepareOpenDevice/安装函数；借用与自有context、原路由回退、shared jetty槽位、错误汇合 |
| [test_python_topology.py](test_python_topology.py) | 运行实际高层wrapper，native模块为桩；类型、mask、异常、复制结果与跨线程实例锁 |
| [test_rfc_integration.py](test_rfc_integration.py) | 只做包含/导出/初始化调用顺序/构建规则静态核对，明确不是构建测试 |

`run_host_tests.py` 已将原 guarded JSON 注入 UT 纳入本机编译执行，并输出源码及生成单元指纹。它默认保留旧的910/950两分支入口，也可显式指定 `--platforms A2 A3 950`。

## 结论如何解读

- `UT_PASS`：该条可用 CPU 单测验证的逻辑已执行通过。
- `UT_PARTIAL`：只完成部分验证，报告备注明确剩余项。
- `SOURCE_AUDIT`：只有源码接线检查，不能替代实际编译/链接。
- `NOT_RUN`：没有所需设备/构建环境，没有执行。
- `EVIDENCE` / `NOT_APPLICABLE`：来源记录或工作量规划，不是运行行为断言。

即使所有已执行 UT 通过，也不能写成“整个 RFC 已全部满足”。以下仍需外部验收：

1. 真实 CANN A2/A3/950 构建、导出符号、pybind编译链接与打包。
2. 真实内存导入和域身份、硬件QP/channel、XSCALE/HNS_1825。
3. 完整 init/finalize、失败回滚、共享jetty和CMO/quiet数据面。
4. 真实左右 IO DIE、plane、RoCE fabric全互通、实时链路。
5. 双层实际流量/trace、吞吐/时延、发现成本、并行效果。

当前双层示例固定两个QP，并把奇数余量给前段；RFC中的任意QP搜索规划器仅是示意，报告明确该差别。

多实例生命周期测试只绑定/释放最小fixture，不能叫真实设备init/finalize验收。Python native数组复制只有源码检查；实际动态运行的是高层wrapper。NPU kernel只提取分段算术，未执行设备搬运。
