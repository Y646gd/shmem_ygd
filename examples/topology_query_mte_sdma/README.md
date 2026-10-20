# MTE + SDMA 拓扑查询用例

该用例在 `aclshmemx_init_attr()` 之前固定设置
`ACLSHMEM_DATA_OP_MTE | ACLSHMEM_DATA_OP_SDMA`（数值 3），初始化成功后调用
`aclshmemx_query_topology(peer, ACLSHMEMX_TOPOLOGY_QUERY_ENGINES, &info)` 查询每个 PE。
它打印接口实际返回的各引擎位，并检查每个非 self 对端是否返回 `engine_mask == 3`。

## Ascend950 注意事项

下文的 `3` 预期及 A3 环境说明针对 A2/A3。950 中 SDMA 仅用于 CMO，当前代码会移除 D2D SDMA 位，因此本固定组合在直接可达条件下的非 self 预期为 `1`。950 用 `-soc_type Ascend950` 构建；RoCE 还需启用并指定现场网卡后端。完整步骤见 [950 部署与调试](../../docs/ascend950_deploy_debug.md)。本例只验证查询，不验证 CMO 或数据搬运。

## 编译

以 A3 / 910C 为例，在项目根目录执行。先加载本机 CANN 环境；SDMA 需要配套驱动、Toolkit 和 A3 ops 包。

```bash
bash scripts/build.sh -soc_type Ascend910B -examples
source install/set_env.sh
export LD_LIBRARY_PATH="$PWD/build/lib:${LD_LIBRARY_PATH:-}"
```

本工程的 `Ascend910B` 构建选项覆盖 A2/A3。也可以使用
`bash scripts/build.sh -soc_type Ascend910B -enable_rdma -examples` 一次构建本用例和三引擎用例。

## 单机双 PE

在同一台服务器的两个终端分别加载 CANN 和上述 SHMEM 环境，然后从项目根目录启动：

```bash
# 终端 A：PE0 使用本机逻辑设备 0。
./build/bin/topology_query_mte_sdma 0 2 0 tcp://127.0.0.1:18899
```

```bash
# 终端 B：PE1 使用本机逻辑设备 1。
./build/bin/topology_query_mte_sdma 1 2 1 tcp://127.0.0.1:18899
```

参数依次为 `PE NPES DEVICE tcp://HOST:PORT`；没有 engine_mask 参数，引擎组合已在本用例中固定。
NPES 至少为 2。所有 PE 必须运行同一个用例，使用相同 NPES 和初始化地址，并在超时前启动。
DEVICE 是各进程所在主机上的可见逻辑设备编号，不是 world PE 编号。

满足 MTE/SDMA 可达条件时，两个进程的跨 PE 输出分别包含：

```text
PE 0 -> 1 engines=3 (MTE=1, SDMA=1, RoCE=0, UDMA=0) expected=3 [PASS]
PE 1 -> 0 engines=3 (MTE=1, SDMA=1, RoCE=0, UDMA=0) expected=3 [PASS]
```

还会打印 self 的实际结果，并标记 `SELF: reported only`，不将其作为跨 PE 校验。
每个进程完成清理后打印自己的 `local result PASS/FAIL`；只有所有进程均返回 0，整组用例才通过。
非 self 的返回值不等于 3、查询失败、初始化失败或清理失败都会使对应进程返回非零。

## 跨机双 PE

两端必须先准备相同版本的代码、CANN/ops 和 SHMEM 构建产物，且所选设备在超节点配置下 MTE/SDMA 可达。
以 `120.9.10.37` 和 `120.9.10.35` 为例，选择两端均可访问的 37 号主机地址和空闲端口：

```bash
# 37 号服务器
./build/bin/topology_query_mte_sdma 0 2 0 tcp://120.9.10.37:18900
```

```bash
# 35 号服务器
./build/bin/topology_query_mte_sdma 1 2 0 tcp://120.9.10.37:18900
```

此时两个 PE 都可以使用自己主机的逻辑设备 0。初始化 TCP 地址只用于协调，并不选择数据面引擎。
若真实拓扑不具备期望关系，用例会打印实际掩码并报告 FAIL；启用某个引擎本身不能保证所有对端可达。

## 代码阅读与范围

- `main.cpp`：固定 MTE + SDMA 组合。
- `../topology_query/engine_query_common.h`：参数检查、ACL/SHMEM 初始化、逐 PE 查询校验和清理。
- 查询 mask 固定为 ENGINES，不请求 950 专属的 LINKS。
- 本用例验证初始化和查询结果，不执行 put/get，也不证明数据面收发或吞吐。

另见 [MTE + SDMA + RoCE 用例](../topology_query_mte_sdma_roce/README.md)。
