# MTE + SDMA + RoCE 拓扑查询用例

该用例在 `aclshmemx_init_attr()` 之前固定设置
`ACLSHMEM_DATA_OP_MTE | ACLSHMEM_DATA_OP_SDMA | ACLSHMEM_DATA_OP_ROCE`（数值 7），
初始化后逐 PE 查询 ENGINES，打印实际返回的各引擎位，并检查每个非 self 对端是否返回 `engine_mask == 7`。

## 编译

以 A3 / 910C 为例，先加载 CANN 环境，并准备配套驱动、Toolkit、A3 ops 和可用的 RoCE 网络。
在项目根目录执行：

```bash
bash scripts/build.sh -soc_type Ascend910B -enable_rdma -examples
source install/set_env.sh
export LD_LIBRARY_PATH="$PWD/build/lib:${LD_LIBRARY_PATH:-}"
```

**必须启用 `-enable_rdma` 才会生成 `topology_query_mte_sdma_roce`。**
这条命令也会构建 MTE + SDMA 用例，二者可以使用同一份库分别运行。
编译支持 RDMA 不会替所有实例启用 RoCE；各用例在初始化参数中选择自己的引擎组合。

## 单机双 PE

两个终端均加载 CANN/SHMEM 环境，从项目根目录分别运行：

```bash
# 终端 A
./build/bin/topology_query_mte_sdma_roce 0 2 0 tcp://127.0.0.1:18901
```

```bash
# 终端 B
./build/bin/topology_query_mte_sdma_roce 1 2 1 tcp://127.0.0.1:18901
```

参数依次为 `PE NPES DEVICE tcp://HOST:PORT`。至少启动两个 PE；所有 PE 使用同一用例、相同 NPES 和初始化地址。
需要为选定设备准备实际可用的 RoCE 配置，单机测试也会进行 RoCE 初始化。

满足三引擎关系时，跨 PE 输出分别包含：

```text
PE 0 -> 1 engines=7 (MTE=1, SDMA=1, RoCE=1, UDMA=0) expected=7 [PASS]
PE 1 -> 0 engines=7 (MTE=1, SDMA=1, RoCE=1, UDMA=0) expected=7 [PASS]
```

其中 `7 = 1 | 2 | 4`，分别对应公开 API 的 MTE、SDMA、ROCE 位。
输出数据来自 `info.engine_mask`，不会把初始化的 7 当成查询结果。
如果只返回 4，输出会显示 MTE=0、SDMA=0、RoCE=1，并标记 FAIL，表示不满足本用例要求的三引擎关系。
查询成功但掩码不匹配，也会返回非零退出码。

## 跨机双 PE

两端需使用相同代码和配套 CANN/ops，均执行上面的 RDMA 构建。设备间要同时满足
MTE/SDMA 可达条件和 RoCE 初始化条件。选用两端均能访问的 PE0 主机地址，例如：

```bash
# 37 号服务器
./build/bin/topology_query_mte_sdma_roce 0 2 0 tcp://120.9.10.37:18902
```

```bash
# 35 号服务器
./build/bin/topology_query_mte_sdma_roce 1 2 0 tcp://120.9.10.37:18902
```

TCP 地址用于初始化协调，RoCE 使用设备网络配置；更改该地址不会强制数据走 RoCE。
两机仅通过 RoCE 连通、而 MTE/SDMA 不可达时，本用例不应通过。

## 判定与范围

- self 结果仅打印，不要求等于跨 PE 的期望值。
- 每个非 self 对端都应返回 7；全部 PE 的 `local result` 均为 PASS 且退出码均为 0，整组用例才通过。
- 初始化、查询、校验或清理失败会打印原因并返回非零；成功初始化后即使查询校验失败，仍会执行 finalize。
- 查询 mask 是 `ACLSHMEMX_TOPOLOGY_QUERY_ENGINES`，不查询 Mesh/Clos，也不执行数据搬运。
- ROCE 位沿用 SHMEM 初始化可达性规则，数据内容正确性和实际网络健康仍需 put/get 用例验证。

两个固定组合的用例共用 [初始化与校验代码](../topology_query/engine_query_common.h)，
各自的 `main.cpp` 明确声明启用的引擎。另见 [MTE + SDMA 用例](../topology_query_mte_sdma/README.md)。
