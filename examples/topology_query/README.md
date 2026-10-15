# 拓扑查询与双层数据消费

`topology_query` 只做初始化和查询，默认 MTE-only；A2/A3 查询 ENGINES，950 可加 LINKS。

构建后，在同一台 950 服务器的两个终端分别启动（所有 PE 使用相同 mask/engine 配置）：

```bash
./build/bin/topology_query 0 2 0 tcp://127.0.0.1:8899 1 1
./build/bin/topology_query 1 2 1 tcp://127.0.0.1:8899 1 1
```

参数依次为 PE、NPES、设备逻辑编号、bootstrap 地址、是否查询 LINKS、引擎 bitmask。A2/A3 将倒数第二项设为 0。可用的引擎配置由平台与构建选项决定，公开位为 MTE=1、SDMA=2、ROCE=4、UDMA=8。输出的空 layers 需结合请求项解释。

需要固定初始化组合并校验返回值时，使用两个独立用例：

| 用例 | 初始化引擎 | 非 self 对端的期望 engine_mask |
| --- | --- | --- |
| [topology_query_mte_sdma](../topology_query_mte_sdma/README.md) | MTE + SDMA | 3 |
| [topology_query_mte_sdma_roce](../topology_query_mte_sdma_roce/README.md) | MTE + SDMA + RoCE | 7 |

二者均只查询 ENGINES，逐位打印实际结果；不匹配会返回非零。RoCE 用例需以 `-enable_rdma` 构建。
每组测试要求选择实际支持相应引擎关系的设备；它们不执行数据搬运。

`topology_layers_demo` 仅在 950 examples 构建中生成。使用 `-multi_link` 构建并选择实际同时具有 layer 0/1 的两个 PE：

```bash
./build/bin/topology_layers_demo -pe 0 -pes 2 -device_ids 0,1 -ipport tcp://127.0.0.1:8899 -op put -elems 1048577
./build/bin/topology_layers_demo -pe 1 -pes 2 -device_ids 0,1 -ipport tcp://127.0.0.1:8899 -op put -elems 1048577
```

此目标复用 `udma_qp_demo` 的初始化、内核、数据校验和清理流程。默认建立两个 QP，并在初始化前为 QP0/QP1 请求 layer0/layer1。初始化后通过 topology API 确認双层物理关系，再用 get_qp_num/get_qp_net_layer 验证**实际** QP；不满足即退出全体 PE，不宣称双层成功。

Host 和 kernel 按相同公式分割长度 N：QP0 使用 `[0, ceil(N/2))`，QP1 使用 `[ceil(N/2), N)`；两段无重叠且覆盖全部数据。每个 AIV 独占一个 QP，提交后执行该 QP 的 quiet；stream 同步及 PE barrier 后消费者校验所有字节。原 `-op get` / `-op put_signal` 也可使用。样例会打印实际 layer、offset、length。

尚需实机运行并采集路径计数器/trace，确认真实流量按层执行；本地源码测试不能替代该步骤。参见 [接口与测试说明](../../docs/api/topology.md)。
