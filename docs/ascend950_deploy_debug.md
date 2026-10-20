# 当前 RFC 实现的 Ascend950 真机部署与调试

## 1. 范围与当前结论

本文随当前源码一起部署，所有命令均从仓库根目录执行。面向 Linux Ascend950，先做单机双卡，再扩展多卡和跨机。

当前没有发现必须为部署而修改的生产逻辑；应先做正式 CANN 构建，再依据实际编译错误或运行日志修复兼容问题。尚未完成真机编译和运行，本文是执行手册，不是通过报告。

当前 RFC 修改包含未提交及新增文件，服务器仅 clone origin 不会得到这些修改。应同步当前整个源码工作目录，排除本机 build/install/dist/package 等生成目录；不要覆盖服务器已有且尚未保存的改动。不要携带本机编译产物。

生产默认读取 `/etc/hccl_rootinfo.json`。便携测试中的模拟 RootInfo、runtime/bootstrap/HCOMM 桩和提取函数测试不属于真实设备验收。不要手工启用 `ACLSHMEM_UNIT_TEST`、`ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO` 或其他测试 override 宏来运行生产样例。

## 2. 环境与资料准备

先按当前仓库 [quickstart.md](quickstart.md) 和 [UDMA 说明](../examples/udma_qp_demo/README.md) 核对版本。仓库列出的 950 基线为 CANN 9.1.0、配套 950 ops、HDK 25.5.6 及以上；实际应使用厂商支持的配套组合。UDMA 所需 HCOMM 接口及 HcommChannelDesc.channelName 必须存在。不要仅凭版本号认定所有接口齐备。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
npu-smi info
uname -a
which bisheng
bisheng --version
cmake --version
python3 --version
printf 'ASCEND_HOME_PATH=%s\n' "$ASCEND_HOME_PATH"
```

自定义 CANN 路径需替换 source 路径。构建依赖 googletest 1.14.x（UT）、nlohmann/json 3.11.3（950）；离线环境按 quickstart 将源码放入 3rdparty 对应目录，保持项目需要的目录结构。Python wheel 另需匹配的 Python 3.10–3.12、TorchNPU 等依赖，第一轮 C++ 验收不要求先构建 wheel。

记录测试设备的逻辑 ID、物理 ID、服务器、网络组、实际可用 layer 和网卡类型。PE 是进程的全局编号，DEVICE 是本机可见逻辑设备编号，二者不能混用。

确认 `/etc/hccl_rootinfo.json` 可读，其中 `topo_file_path` 指向的文件存在。核对设备身份、local ID、端口、EID/IP、plane、网络实例与现场一致。由现场配置负责人提供真实文件；不要把 tests/topology 中生成的模拟文件覆盖到 /etc。即使 MTE-only，当前 950 初始化仍建立共用 topology context，因此发现资料也是前置条件。

## 3. 第一轮构建：950 + UDMA multi-link

```bash
mkdir -p deploy_logs
set -o pipefail
bash scripts/build.sh -soc_type Ascend950 -examples -uttests -multi_link 2>&1 | tee deploy_logs/build-950.log
source install/set_env.sh
export LD_LIBRARY_PATH="$PWD/build/lib:${LD_LIBRARY_PATH:-}"
ls build/bin
ldd build/bin/topology_query
```

预期包含 topology_query、topology_layers_demo、udma_qp_demo 和 UT 目标。检查 ldd 没有 not found，而且实际加载当前仓库的库。切换配置遇到旧 CMake 缓存问题时，可在保存日志后用 build.sh 的 -clean 重新构建；该选项会删除旧 build。

UDMA 不需要 -enable_rdma。显式 layer 策略需要 -multi_link，且不能与 -enable_relay 同时启用。CMake 自动设置 ACLSHMEM_SOC_950 等宏，无需手工改源码宏。

需要 RoCE 时，确认网卡后再增加 -enable_rdma -rdma_backend XSCALE 或 HNS_1825，不要猜测后端。例如：

```bash
bash scripts/build.sh -soc_type Ascend950 -examples -uttests -multi_link -enable_rdma -rdma_backend XSCALE
```

每次重建后重新加载 install/set_env.sh，并核对动态库加载路径。

## 4. 两个终端的公共运行环境

两个终端均进入同一仓库，并执行以下配置；每组测试换一个空闲端口及 session，所有参与 PE 使用相同配置。

```bash
source /usr/local/Ascend/ascend-toolkit/set_env.sh
source install/set_env.sh
export LD_LIBRARY_PATH="$PWD/build/lib:${LD_LIBRARY_PATH:-}"
export SHMEM_LOG_LEVEL=DEBUG
export SHMEM_LOG_TO_STDOUT=1
export SHMEM_UID_SESSION_ID=950-rfc-local-18899
mkdir -p deploy_logs
```

不要同时运行多组占用相同设备/端口的任务。跨机测试中 localhost 要换为所有节点可访问的 PE0 主机地址，并保证 NPES、引擎配置、QP 数、策略、session 一致。udma_qp_demo/run.sh 是本机启动器，不能把它的 -pes 直接当作跨机启动功能。

## 5. 先跑初始化与查询

选择实际 MTE 可达的逻辑设备 0、1，两个终端分别执行：

```bash
# 终端 A
./build/bin/topology_query 0 2 0 tcp://127.0.0.1:18899 2 1 > deploy_logs/query-pe0.log 2>&1
```

```bash
# 终端 B
./build/bin/topology_query 1 2 1 tcp://127.0.0.1:18899 2 1 > deploy_logs/query-pe1.log 2>&1
```

每条命令结束后在对应终端立即执行 `echo $?`，记录各 PE 退出码。参数顺序：PE、NPES、DEVICE、bootstrap 地址、links 模式、十进制 engine_config。links=0 仅 ENGINES，1 同时查询 LINKS，2 为含负例的矩阵模式。最后参数 1 是 MTE-only，9 是 MTE+UDMA，13 是 MTE+ROCE+UDMA（需要 RDMA 构建和现场资源）。不要把 engine_config 与查询 mask 混为一谈。

重要：矩阵模式打印结果，**不自动断言每个 status/字段是否正确**，查询负例也不应统一要求 status=0。进程退出码为 0 不等于矩阵合同全部通过，需要逐项审阅日志。

| 项目 | 判定 |
| --- | --- |
| init_status | 正例所有 PE 为 0；初始化失败后不应继续消费查询输出 |
| query=1 | ENGINES，net_layer_count=0、net_layers=[] |
| query=2 | LINKS，engine_mask=0 |
| query=3 | 同时返回两部分 |
| query=0/4、越界 PE、空输出、ABI/size 错误 | 按公开接口合同返回错误；失败输出不用于比较 |
| self LINKS | [] |
| LINKS 内容 | 仅 0/1/3，排序去重，并对照真实端点资料核验 |
| MTE-only LINKS | 仍能返回物理层描述，不依赖启用 UDMA |

950 self 没有 UDMA D2D 位，SDMA 也不作为 D2D 位返回。直接可达且启用 MTE 的 self 通常为 1；配置 RDMA 时按现有能力规则还可带 ROCE 位。远端 UDMA 要满足后端直接可达条件、引擎启用、manager 存在并且实际 QP0 路由 getter 成功。LINKS 不能替代这些条件。

随后两端一起把最后参数改为 9，另选端口/session 重跑。比较 LINKS 是否保持初始化物理描述语义；不要预设所有设备对一定支持 UDMA。

## 6. UDMA 数据面

以下命令依次运行，每组等待所有 PE 完成后再运行下一组；设备编号按现场替换。

```bash
bash examples/udma_qp_demo/run.sh -pes 2 -device_ids 0,1 -qp_count 2 -op put
bash examples/udma_qp_demo/run.sh -pes 2 -device_ids 0,1 -qp_count 2 -op get
bash examples/udma_qp_demo/run.sh -pes 2 -device_ids 0,1 -qp_count 2 -op put_signal
bash examples/udma_qp_demo/run.sh -pes 2 -device_ids 0,1 -qp_count 2 -shared_jetty 1 -op put
```

要求每个 PE 数据校验 PASS，PutSignal 还要 signal 校验 PASS；所有进程退出码为 0，quiet/finalize 正常完成。保存每组完整标准输出和错误输出。脚本要求已经生成 install/set_env.sh；显式 source 正确 CANN，避免使用脚本中的备用 CANN 路径。

## 7. 双层实际 QP 与数据验证

仅选择真实具有 layer 0 和 1 的设备对，且已用 -multi_link 构建。两个终端分别执行，使用相同 session 和空闲端口：

```bash
# 终端 A
./build/bin/topology_layers_demo -pe 0 -pes 2 -device_ids 0,1 -ipport tcp://127.0.0.1:18900 -op put -elems 1048577
```

```bash
# 终端 B
./build/bin/topology_layers_demo -pe 1 -pes 2 -device_ids 0,1 -ipport tcp://127.0.0.1:18900 -op put -elems 1048577
```

检查物理 LINKS 含 0/1、实际 QP 数为 2、QP0/1 实际层分别为 0/1、分片覆盖完整且数据正确。不满足条件应退出，不应报告双层成功。再按同样方式测试 get、put_signal。

当前样例沿用 ceil(N/2) 的首段分配，不是 RFC 举例中的任意 QP 通用选择器。要证明真实流量按层经过，还需现场 trace/链路计数器，查询层信息不能替代流量证据。

## 8. UT、RoCE、CMO 和 Python 的后续验收

```bash
bash scripts/run.sh -ranks 2 -gnpus 2 -fnpu 0
```

运行前读 scripts/run.sh：它运行设备 UT 及存在时的 topology UT，还检查 HBM 使用变化；使用的卡数必须符合现场资源。该入口不自动包含 tests/topology/run_rfc_ut.py 的全部便携测试。后者验证 CPU/桩路径，不能替代设备验收。

RoCE 使用 examples/rdma_qp_demo 的对应后端 Put/Get；XSCALE 另可测聚合操作。950 SDMA 是 CMO 路径，必须另测 CMO 与 quiet，不能用 SDMA D2D put 替代。

Python 需要构建当前修改后的 wheel，安装到隔离环境并确认 shmem-config --backend 选择 950；先跑 tests/package_smoke/run_install_smoke.sh，再用真实初始化实例调用 query_topology。检查 list 在 finalize 后仍保留数值、重复 init/finalize、多实例切换和异常返回。wheel 构建会涉及额外后端和依赖，见 compilation_build_guide.md；import 成功不等于设备查询通过。

## 9. 排错顺序与代码入口

| 现象 | 优先核查 | 代码 |
| --- | --- | --- |
| 编译缺头文件/符号 | CANN 路径、版本、HCOMM 接口、第三方依赖；保留首个错误 | scripts/build.sh、CMakeLists.txt |
| RootInfo/拓扑解析失败 | 物理设备映射、文件权限和 topo_file_path；不要先改 matcher 放宽条件 | src/host/transport/topo/topology_context.cpp、topo_reader.cpp |
| allgather 等待 | 所有 PE 是否启动，地址/session/NPES 是否一致，最先失败的 PE 日志 | topology_context.cpp::Agree/Initialize |
| HCOMM 创建失败 | 实际运行库、EID 和设备网络配置、资源余量 | device_udma_transport_manager.cpp::OpenDevice/PrepareOpenDevice |
| 初始化 -100 | 结合具体阶段判断：远端无引擎、无路由或其他不支持；不是统一当作查询错误 | shmem_init_backend.cpp::validate_peer_reachability |
| LINKS 正确但 UDMA 位缺失 | 直接可达判定、配置、manager、实际 QP0 路由 | mem_entity_default.cpp::CanReachDataOperators |
| engine 位异常 | HYBM 能力、topo_list、transport 到公开位转换 | shmem_init_backend.cpp::reach_info_init/query_topology |
| 双层验证失败 | 实际层 0/1、multi-link、QP 数及 QP 实际层 | examples/topology_query/topology_plan.h |
| 数据不对/quiet 挂住 | 先看其他 PE 失败、实际 QP、分片与同步，不要仅根据 LINKS 判断数据面 | examples/udma_qp_demo、src/device |
| finalize/重复初始化异常 | 资源释放顺序、借用 context 生命周期、残留进程 | shmem_init.cpp、shmem_init_backend.cpp |

日志变量见 [日志说明](debug/log_debug.md)。开启 SHMEM_LOG_TO_STDOUT=1 后日志到控制台；要落文件可使用 shell 重定向，或关闭该选项并设置 SHMEM_LOG_PATH。调试构建可加 -debug，但当前 CMake 仍有全局链接 -s；若 gdb 看不到函数符号，先检查二进制符号表，必要时单独调整调试构建的 strip 行为，不要承诺 -debug 一定保留完整符号。

跨机故障先收集所有 PE 日志，不要只看 PE0。清理时仅终止确认属于本次任务的进程，不要批量杀其他用户任务。

## 10. 交付测试结果需要保留什么

每组记录：源码版本和未提交改动快照/文件哈希、完整构建命令及日志、CANN/HDK/固件版本、逻辑/物理设备映射、真实拓扑文件哈希、网卡后端、引擎配置、QP 策略、bootstrap/session、每个 PE 日志和退出码。真实端点资料按现场权限保存，不要直接上传敏感网络配置。

结果分开标记：BUILD_PASS、QUERY_REVIEW_PASS、DATA_PASS、TRACE_CONFIRMED、NOT_RUN。查询打印正常、便携 UT 通过或某一个 PE 返回 0，都不能代替整个 RFC 的真机验收。
