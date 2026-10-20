# Ascend 950 配置驱动拓扑查询：完整测试链路与代码

> 2026-10-19补齐后的执行入口见[完整本机验收说明](../README_ACCEPTANCE.md)。本页原函数与runner代码快照保留；当前源代码已增加真实远端能力门禁、扩展加载分支、UDMA路由、A2/A3平台分支和错误状态记录。旧文中的“本配置模式不接受UDMA”“无远端能力仅由桩拒绝”描述适用于原快照，当前行为以新说明和工作区源码为准。

本文按 2026-10-19 工作区代码核对，说明 2026-10-16 导出的 **950 配置驱动测试**。接口的实际名称为 `aclshmemx_query_topology()`。此文新增，原 readme、输入 JSON、测试代码和旧结果均保留。

本轮结果：两份拓扑 × 三种初始化配置 × 16 个源 PE × 16 个目标 PE × 三种 query_mask，共 **96 个源 PE/配置场景、4,608 次成功查询**。这是此前保存的执行结果，不表示 2026-10-19 又进行了设备测试。

## 1. 先看本次测试究竟执行了什么

```text
950-capability-cases.json
  ├─ engine_config：公开初始化配置 0x3 / 0x5 / 0x7
  ├─ direct_reachable：显式指定每个目标的 D，本轮全部 true
  └─ RootInfo + TopoInfo：提供物理网络描述
          ↓
run.py：生成隔离头文件和生产函数体，clang++ 编译 ACLSHMEM_SOC_950
          ↓
main.cpp：RunCase
  ├─ 真实 C++ parser + BuildLocalEndpoints，构造远端交换 payload
  ├─ TopologyContext::Initialize，调用 replay allgather 桩
  ├─ 最小 backend fixture 绑定实例并持有 context
  │
  ├─ 引擎能力链
  │    engine_config
  │      → ConfigureEngines（create_entity 的原配置转换片段）
  │      → CanReachDataOperators（原生产函数体；SdmaReaches 用 D 桩）
  │      → StoreCapability（reach_info_init 的原位装配片段）
  │      → fixture.state.topo_list[peer]
  │
  ├─ 网络层链
  │    RootInfo / TopoInfo / 各 PE SyncEndpoint
  │      → build_topology_state（原生产函数体）
  │      → TopoQuerier::GetReachableNetLayers（直接编译生产源）
  │      → topology_net_layers_by_pe
  │
  └─ 设置测试实例 initialized
       → aclshmemx_query_topology（原生产函数体）
       → backend::query_topology（原生产函数体）
       → status / engine_mask / net_layer_count / net_layers
       → C++ Require 断言 + JSON 日志
          ↓
generate_950_capability_report.py：解析日志、追加断言、生成 JSON/CSV/HTML
          ↓
export_950_excel.py：从结果 JSON 写入 Excel，逐条核对 engine_mask/status
```

**没有调用完整 `aclshmemx_init_attr()`；没有执行真实 heap/entity/QP 创建、内存导入或数据搬运。** 模拟类与容器不具备生产对象的完整布局；实际执行的是提取出的生产函数体及直接编译的拓扑源文件。`g_state.is_aclshmem_initialized=true` 由 fixture 设置，不是设备初始化成功的证据。

## 2. 文件入口与职责

以下链接相对于本 README，可直接打开源码。

| 文件 | 用途 |
| --- | --- |
| [run.py](run.py) | 完整编译调度；生成 runtime/backend/能力类替身；从当前生产源提取函数体 |
| [main.cpp](main.cpp) | 完整 C++ 执行入口、远端交换回放、配置模式、断言与日志 |
| [950-capability-cases.json](../results/950-20261016/950-capability-cases.json) | 96 个配置驱动场景；不是旧 internal_transport_mask 模式 |
| [mem_entity_default.cpp](../../../src/host/entity/mem_entity_default.cpp) | 生产 CanReachDataOperators；950 排除 SDMA D2D |
| [shmem_init_backend.cpp](../../../src/host/init/backends/shmem_init_backend.cpp) | 配置转换、reach_info_init 装配、网络层缓存构建、backend query |
| [shmem_init.cpp](../../../src/host/init/shmem_init.cpp) | public query、参数/布局/初始化校验和全局锁 |
| [topology_context.cpp](../../../src/host/transport/topo/topology_context.cpp) | 直接编译的真实拓扑发现、端点生产、MessagePack 和交换控制流 |
| [topo_reader.cpp](../../../src/host/transport/topo/topo_reader.cpp) | 直接编译的真实 parser、物理匹配与网络层枚举 |
| [generate_950_capability_report.py](../results/950-20261016/generate_950_capability_report.py) | 日志解析、额外结果断言及图表/CSV 生成 |
| [export_950_excel.py](../results/950-20261016/export_950_excel.py) | 完整 Excel 导出代码 |

## 3. 两份输入和 D 的来源

| 输入/条件 | 本次使用方式 |
| --- | --- |
| [root_info-biaoka.json](root_info-biaoka.json) | 用户 RootInfo 原文件；其 topo_file_path 在 Mac 上重定位为本目录同名 atlas_350_3.json |
| [atlas_350_3.json](atlas_350_3.json) | biaoka 对应的物理边文件，之前从 driver 源原样复制 |
| [root_info-ubx.json](root_info-ubx.json) | 实际内容是 peer_list/edge_list 物理 TopoInfo，不能直接作为 RootInfo |
| [ubx-synthetic-rootinfo.json](../results/950-20261016/ubx-synthetic-rootinfo.json) | UBX 另生成的模拟 RootInfo；topo_file_path 指向用户 UBX 原物理边文件 |
| D / direct_reachable | 每个目标由 fixture 显式给出；本轮均 true。JSON 中有某层不表示内存段已导入，也不能用于推导 D |

UBX 模拟假设：PE=device_id=local_id；按照原 layer 0 边端口提供模拟 EID，layer 0 使用同一非空测试网络实例与 plane；layer 3 使用 192.0.2.0/24 文档测试 IPv4 地址，同一非空全互通 RoCE 网络实例与 plane。原始物理边没有改动，但地址/plane/网络实例假设会影响 LINKS，不能视作真实设备 RootInfo 的证明。

本配置模式仅接受公开 MTE/SDMA/ROCE 配置（0..7），不覆盖 UDMA。RDMA 资源可用是 fixture 前提，RDMA 按配置使能产生位；未验证 XSCALE/HNS_1825 真实 backend 初始化。CMO 资源和 completion 数据面也不在这条执行链中。

## 4. 一条实际 fixture

下面是套件中的原始第一条。`expected_engine_masks` 仅在结果断言阶段读取，不参与能力生产或层缓存生成。

```json
{
  "name": "biaoka_MTE_SDMA_source_0",
  "root_info": "/Users/Admin/Projects/shmem-rfc/tests/topology/cpp_json_stub/root_info-biaoka.json",
  "my_pe": 0,
  "engine_config": 3,
  "direct_reachable": [
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true,
    true
  ],
  "expected_engine_masks": [
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1
  ]
}
```

`engine_config=3` 是公开 MTE+SDMA。950 D2D 查询结果期望为 MTE=1；CMO 使能不能当作 SDMA D2D 能力。本模式禁止同时提供 internal_transport_mask(s)，防止不小心绕过生产能力生成。

## 5. 构建方式与生产代码的编译边界

`run.py` 实际编译四个翻译单元：

```text
临时 queries.cpp                       原 public/backend query 与 build 函数体
本目录 main.cpp                        测试入口，包含生成的 capability_fixture.h
src/host/transport/topo/topo_reader.cpp  生产原文件
src/host/transport/topo/topology_context.cpp 生产原文件
```

关键编译参数为 `-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread -DACLSHMEM_SOC_950`。宏 `ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO` 将 `/etc/hccl_rootinfo.json` 重定向到临时文件，原用户 JSON 和 `/etc` 都不写入。

生成头文件 `capability_fixture.h` 包含一个最小 `shm::MemEntityDefault` 替身：options_ 使用真实 HYBM 类型；`SdmaReaches(peer)` 返回 fixture 的 direct；transportManager_ 默认为空，RouteStub 的 QP getter 返回 NOT_SUPPORTED。随后附加生产 CanReachDataOperators 的完整函数体。本轮没有 UDMA 配置，因此不以这个 QP 桩验证 UDMA。

`ConfigureEngines`、`StoreCapability` 是**测试包装名称**，不是新增生产接口。其内部代码每次运行从生产源提取，不维护第二份能力算法。提取器通过函数名、片段锚点与花括号定位，并使用 `#line` 对函数体标注原源路径；这是当前源码形态下的轻量提取，不是通用 C++ parser。锚点变化、提取不完整或编译报错都会使 runner 失败。

## 6. 引擎能力链：实际执行的生产代码

### 6.1 ConfigureEngines 中的配置转换片段

源位置：生产 `create_entity()`。包装函数在以下代码后返回 options；没有执行整个 create_entity。

```cpp
    hybm_options options{};
    options.bmType = hybm_type::HYBM_TYPE_AI_CORE_INITIATE;
    options.bmDataOpType = static_cast<hybm_data_op_type>(HYBM_DOP_TYPE_MTE);
    if (attributes->option_attr.data_op_engine_type & ACLSHMEM_DATA_OP_ROCE) {
        auto temp = static_cast<uint32_t>(options.bmDataOpType) | HYBM_DOP_TYPE_DEVICE_RDMA;
        options.bmDataOpType = static_cast<hybm_data_op_type>(temp);
    }
    if (attributes->option_attr.data_op_engine_type & ACLSHMEM_DATA_OP_SDMA) {
        auto temp = static_cast<uint32_t>(options.bmDataOpType) | HYBM_DOP_TYPE_DEVICE_SDMA;
        options.bmDataOpType = static_cast<hybm_data_op_type>(temp);
    }
    if (attributes->option_attr.data_op_engine_type & ACLSHMEM_DATA_OP_UDMA) {
        auto temp = static_cast<uint32_t>(options.bmDataOpType) | HYBM_DOP_TYPE_DEVICE_UDMA;
        options.bmDataOpType = static_cast<hybm_data_op_type>(temp);
    }
```

公开配置 SDMA=2、ROCE=4，转换为内部 HYBM SDMA=4、RDMA=2；基础 MTE=1 始终加入。不能把配置值原样当作内部 transport 位。

### 6.2 CanReachDataOperators 原函数体

源位置：`src/host/entity/mem_entity_default.cpp`。此次编译定义 ACLSHMEM_SOC_950，因此 `#ifndef ACLSHMEM_SOC_950` 中的 SDMA D2D 分支不进入编译。

```cpp
hybm_data_op_type MemEntityDefault::CanReachDataOperators(uint32_t remoteRank) const noexcept
{
    uint32_t supportDataOp = 0U;
    bool sdmaReach = SdmaReaches(remoteRank); // SDMA reaches mean MTE reaches too
    if (sdmaReach) {
        supportDataOp |= HYBM_DOP_TYPE_MTE;
    }
#ifndef ACLSHMEM_SOC_950
    // 950 SDMA supports CMO only; it is not a PE-to-PE D2D engine.
    if (sdmaReach && ((options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_SDMA) != 0)) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_SDMA;
    }
#endif
    if ((options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) != 0) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_RDMA;
    }
    // UDMA requires an actual QP0 route; an enable bit alone is insufficient.
    // Self has no UDMA route. Preserve the existing MTE and enabled RDMA bits.
    uint32_t netLayer = 0;
    if (remoteRank != options_.rankId && sdmaReach && ((options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_UDMA) != 0) &&
        transportManager_ != nullptr &&
        transportManager_->GetQpNetLayer(ACLSHMEM_DATA_OP_UDMA, remoteRank, 0, netLayer) == ACLSHMEM_SUCCESS) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_UDMA;
    }

    return static_cast<hybm_data_op_type>(supportDataOp);
}
```

因此 D=1、没有 UDMA 时：配置 0x3 → HYBM 能力 0x1；配置 0x5/0x7 → HYBM 能力 0x3（内部 MTE+RDMA）。self 保留既有 MTE 和使能 RDMA 规则，不因 self 自动删除 RDMA。

### 6.3 StoreCapability 中的原位装配片段

源位置：生产 reach_info_init。此片段把刚算出的能力写入初始为零的 topo_list；它没有创建实际内存映射或 quiet 资源。

```cpp
        if (reaches_types & HYBM_DOP_TYPE_MTE) {
            host_state->topo_list[i] |= ACLSHMEM_TRANSPORT_MTE;
        }
        if (reaches_types & HYBM_DOP_TYPE_DEVICE_RDMA) {
            host_state->topo_list[i] |= ACLSHMEM_TRANSPORT_ROCE;
        }
        if (reaches_types & HYBM_DOP_TYPE_DEVICE_SDMA) {
            host_state->topo_list[i] |= ACLSHMEM_TRANSPORT_SDMA;
        }
        if (reaches_types & HYBM_DOP_TYPE_DEVICE_UDMA) {
            host_state->topo_list[i] |= ACLSHMEM_TRANSPORT_UDMA;
        }
```

这一步之后，本轮三种配置的 topo_list 内部值为 0x1 / 0x3 / 0x3。公开查询再把内部 RDMA/ROCE=2 转为公开 ROCE=4，最终得到 0x1 / 0x5 / 0x5。

## 7. 网络层链与 bootstrap 桩

`PreparePeers()` 对每个设备调用真实 ParseRootInfo、ParseTopoInfo、GetLocalId、GetEidCount、BuildLocalEndpoints，将真实 C++ 端点序列化为 MessagePack。`Replay::Gather` 按阶段回放这些远端 payload，并检查本地发送内容。没有把 expected_layers 填入交换数据或缓存。

| 阶段 | 本 runner 的 allgather 替身 |
| --- | --- |
| 1 | 解析与本地准备状态一致性 |
| 2 | EID slot count |
| 3 | local ID |
| 4 | MessagePack 长度 |
| 5 | 接收/发送缓冲准备状态一致性 |
| 6 | MessagePack 内容，逐 PE 回放并检查本地数据 |
| 7 | 解码结果一致性 |
| 8 | build_topology_state 完成状态一致性 |

这是本桩执行的 8 个阶段；不能等同完整 SHMEM 初始化的 collective 总数。状态阶段复制本地状态到测试参与者，不模拟真实多进程失联或任意远端故障。

### 7.1 TopologyContext::Initialize 原函数体

```cpp
int TopologyContext::Initialize(
    int32_t user_device_id, uint32_t rank, uint32_t npes, aclshmemi_bootstrap_handle* bootstrap)
{
    // All PEs enter the same phases. No local exception can skip a collective.
    std::vector<uint32_t> counts, lengths;
    std::vector<uint8_t> blob, send, received;
    uint32_t local_id = 0, eid_count = 0, length = 0, max_length = 0;
    int status = Prepare([&]() -> int {
        if (rank >= npes || npes == 0 || npes > ACLSHMEM_MAX_PES || user_device_id < 0) {
            return ACLSHMEM_INVALID_VALUE;
        }
        int32_t logic_id = -1, physical_id = -1;
        // Keep the original runtime mapping semantics: the physical-ID API accepts user ID.
        if (DlAclApi::RtGetLogicDevIdByUserDevId(user_device_id, &logic_id) != 0 || logic_id < 0 ||
            DlAclApi::AclrtGetPhyDevIdByLogicDevId(user_device_id, &physical_id) != 0 || physical_id < 0) {
            return ACLSHMEM_DL_FUNC_FAILED;
        }
        physical_device_id = static_cast<uint32_t>(physical_id);
        my_pe = rank;
        if (!TopoReader::ParseRootInfo(physical_device_id, root_info) ||
            !TopoReader::ParseTopoInfo(root_info.topoFilePath, topo_info) ||
            !TopoReader::GetLocalId(root_info, physical_device_id, local_id) ||
            !TopoReader::GetEidCount(root_info, eid_count)) {
            return ACLSHMEM_INNER_ERROR;
        }
        blob = nlohmann::json::to_msgpack(nlohmann::json(BuildLocalEndpoints(root_info, topo_info)));
        if (blob.size() > static_cast<size_t>(std::numeric_limits<int>::max()) / npes) {
            return ACLSHMEM_INVALID_VALUE;
        }
        length = static_cast<uint32_t>(blob.size());
        counts.resize(npes);
        lengths.resize(npes);
        rank_to_local_id.resize(npes);
        rank_to_sync_endpoints.resize(npes);
        return ACLSHMEM_SUCCESS;
    });
    status = Agree(status, npes, bootstrap);
    if (status != ACLSHMEM_SUCCESS) {
        return status;
    }
    auto gather = [&](const void* source, void* destination, int bytes) {
        return bootstrap->allgather(source, destination, bytes, bootstrap) == 0 ?
            ACLSHMEM_SUCCESS : ACLSHMEM_BOOTSTRAP_ERROR;
    };
    // Preserve the original EID-count/local-ID/length/blob discovery order.
    if (gather(&eid_count, counts.data(), sizeof(eid_count)) != ACLSHMEM_SUCCESS ||
        gather(&local_id, rank_to_local_id.data(), sizeof(local_id)) != ACLSHMEM_SUCCESS ||
        gather(&length, lengths.data(), sizeof(length)) != ACLSHMEM_SUCCESS) {
        return ACLSHMEM_BOOTSTRAP_ERROR;
    }
    status = Prepare([&]() -> int {
        max_eid_slot_count = *std::max_element(counts.begin(), counts.end());
        max_length = *std::max_element(lengths.begin(), lengths.end());
        if (max_eid_slot_count == 0 || max_length == 0 ||
            max_length > static_cast<size_t>(std::numeric_limits<int>::max()) / npes) {
            return ACLSHMEM_INVALID_VALUE;
        }
        send.resize(max_length);
        std::copy(blob.begin(), blob.end(), send.begin());
        received.resize(static_cast<size_t>(max_length) * npes);
        return ACLSHMEM_SUCCESS;
    });
    status = Agree(status, npes, bootstrap);
    if (status != ACLSHMEM_SUCCESS) {
        return status;
    }
    status = gather(send.data(), received.data(), static_cast<int>(max_length));
    if (status != ACLSHMEM_SUCCESS) {
        return status;
    }
    status = Prepare([&]() -> int {
        for (uint32_t pe = 0; pe < npes; ++pe) {
            const auto* begin = received.data() + static_cast<size_t>(pe) * max_length;
            rank_to_sync_endpoints[pe] =
                nlohmann::json::from_msgpack(begin, begin + lengths[pe]).get<std::vector<SyncEndpoint>>();
        }
        return ACLSHMEM_SUCCESS;
    });
    return Agree(status, npes, bootstrap);
}
```

### 7.2 build_topology_state 原函数体

```cpp
int aclshmemi_init_backend::build_topology_state(uint64_t instance_id)
{
#ifdef ACLSHMEM_SOC_950
    std::lock_guard<std::mutex> lock(entity_map_mutex_);
    const auto it = entity_map_.find(instance_id);
    if (it == entity_map_.end() || it->second == nullptr) {
        return ACLSHMEM_NOT_INITED;
    }
    auto* elem = it->second;
    const auto& host = *elem->entity_host_state;
    int status = ACLSHMEM_SUCCESS;
    try {
        if (elem->topology_context == nullptr) {
            status = ACLSHMEM_NOT_INITED;
        } else {
            const auto& context = *elem->topology_context;
            shm::transport::TopoQuerier querier(context.root_info, context.topo_info, host.mype,
                context.rank_to_local_id, context.rank_to_sync_endpoints);
            std::vector<std::vector<aclshmemx_net_layer_t>> layers_by_pe(host.npes);
            for (int pe = 0; pe < host.npes; ++pe) {
                std::vector<uint32_t> layers;
                querier.GetReachableNetLayers(pe, layers);
                for (const auto layer : layers) {
                    layers_by_pe[pe].push_back(static_cast<aclshmemx_net_layer_t>(layer));
                }
            }
            elem->topology_net_layers_by_pe = std::move(layers_by_pe);
        }
    } catch (const std::bad_alloc&) {
        status = ACLSHMEM_MALLOC_FAILED;
    } catch (const std::exception&) {
        status = ACLSHMEM_INNER_ERROR;
    }
    return shm::transport::TopologyContext::Agree(status, host.npes, elem->entity_boot_handle);
#else
    (void)instance_id;
    return ACLSHMEM_SUCCESS;
#endif
}
```

GetReachableNetLayers 内部调用关系使用直接编译的 topo_reader.cpp；Mesh/Clos 附件和 RoCE 网络实例匹配都在该生产文件中。LINKS 与 engine_config 独立。此套件不创建 UDMA QP，也不通过实际 QP 层生产物理网络层集合。

## 8. public query 与返回装配代码

### 8.1 public 入口原函数体

v1 最小结构体长度为 64，ABI/偏移 static_assert 同时由 runner 提取编译。下面是调用时实际执行的入口函数体。

```cpp
int aclshmemx_query_topology(int pe, uint32_t query_mask, aclshmemx_topology_t* topology)
{
    if (topology == nullptr) {
        return ACLSHMEM_INVALID_PARAM;
    }
    constexpr uint32_t defined_mask = ACLSHMEMX_TOPOLOGY_QUERY_ENGINES | ACLSHMEMX_TOPOLOGY_QUERY_LINKS;
    if (query_mask == 0 || (query_mask & ~defined_mask) != 0 ||
        topology->struct_size < TOPOLOGY_V1_SIZE || topology->abi_version != ACLSHMEMX_TOPOLOGY_ABI_VERSION) {
        return ACLSHMEM_INVALID_VALUE;
    }
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (init_manager == nullptr || g_instance_ctx == nullptr || !g_state.is_aclshmem_initialized) {
        return ACLSHMEM_NOT_INITED;
    }
    if (pe < 0 || pe >= g_state.npes) {
        return ACLSHMEM_INVALID_VALUE;
    }
    return init_manager->query_topology(g_instance_ctx->id, pe, query_mask, topology);
}
```

### 8.2 backend query 原函数体

```cpp
int aclshmemi_init_backend::query_topology(
    uint64_t instance_id, int pe, uint32_t query_mask, aclshmemx_topology_t* out)
{
    std::lock_guard<std::mutex> lock(entity_map_mutex_);
    const auto it = entity_map_.find(instance_id);
    if (it == entity_map_.end() || it->second == nullptr) {
        return ACLSHMEM_NOT_INITED;
    }
    const auto* elem = it->second;
#ifndef ACLSHMEM_SOC_950
    if ((query_mask & ACLSHMEMX_TOPOLOGY_QUERY_LINKS) != 0) {
        return ACLSHMEM_NOT_SUPPORTED;
    }
#endif
    uint32_t engine_mask = 0;
    if ((query_mask & ACLSHMEMX_TOPOLOGY_QUERY_ENGINES) != 0) {
        const auto transport = elem->entity_host_state->topo_list[pe];
        // Internal ROCE/SDMA positions differ from the public data-op bits.
        if (transport & ACLSHMEM_TRANSPORT_MTE) engine_mask |= ACLSHMEM_DATA_OP_MTE;
        if (transport & ACLSHMEM_TRANSPORT_SDMA) engine_mask |= ACLSHMEM_DATA_OP_SDMA;
        if (transport & ACLSHMEM_TRANSPORT_ROCE) engine_mask |= ACLSHMEM_DATA_OP_ROCE;
        if (transport & ACLSHMEM_TRANSPORT_UDMA) engine_mask |= ACLSHMEM_DATA_OP_UDMA;
    }
    out->engine_mask = engine_mask;
    out->net_layer_count = 0;
    out->net_layers = nullptr;
#ifdef ACLSHMEM_SOC_950
    if ((query_mask & ACLSHMEMX_TOPOLOGY_QUERY_LINKS) != 0) {
        const auto& layers = elem->topology_net_layers_by_pe[pe];
        out->net_layer_count = static_cast<uint32_t>(layers.size());
        out->net_layers = layers.empty() ? nullptr : layers.data();
    }
#endif
    return ACLSHMEM_SUCCESS;
}
```

公开结果数组借用自 fixture 实例缓存。桩在读完、复制和打印数组后释放 fixture，不在析构后使用指针。此处不是 CMO 使能查询；返回的是 PE-to-PE D2D 能力。

## 9. C++ 断言、日志与结果生成

主循环对每个目标执行 mask=1/2/3，检查 status 成功、请求的引擎结果与期望相等、未请求字段为零、reserved 保留、空层数组指针为 nullptr。本 fixture 没有 expected_layers，因此层内容不进行独立的物理预期断言；日志中的 links_expectation_checked=false 如实表达这个边界。

runner 还检查未初始化、空结果、PE=-1、mask=0、短布局。这里不声称覆盖所有错误组合，ABI 版本不匹配和全部未知 mask 等额外用例位于其他 Host UT。它们不计入本报告的 4608 条成功调用。

后处理 generate_950_capability_report.py 另行检查：

- 日志末尾为 96 cases PASS，成功记录数为 4608。
- engine_state_source 必须为 production_capability_with_stub_D，配置与组名一致。
- ENGINES/合并的 engine_mask=1/5/5，LINKS-only 为 0，任何返回都不得含 SDMA=2。
- 请求 LINKS 时的层数组与前次相同物理输入的 baseline 计算结果一致。这验证改变引擎配置不改变 LINKS，不是独立证明物理组网正确。

Excel 从修正后的 950-capability-results.json 读取，生成“结果汇总”“完整接口返回”“打桩条件与边界”三个 sheet；导出后检查 ZIP/XML 结构和 4608 条明细，并逐条对照原始 engine_mask/status。导出脚本不执行查询，也不计算拓扑。

## 10. 本次观察到的返回

以下均要求本轮 D=1 和 RDMA 资源可用的 fixture 前提，status=0。

| 初始化配置 | 内部 HYBM/transport 能力 | query=1 的 engine_mask | query=2 的 engine_mask | query=3 的 engine_mask |
| --- | --- | --- | --- | --- |
| MTE+SDMA 0x3 | 0x1 | 0x1 | 0 | 0x1 |
| MTE+ROCE 0x5 | 0x3 | 0x5 | 0 | 0x5 |
| MTE+SDMA+ROCE 0x7 | 0x3 | 0x5 | 0 | 0x5 |

query=1 的网络层字段为 0/nullptr。query=2/3 时，biaoka 组内非 self={0,3}、跨组={3}；UBX 模拟 RootInfo 组内={0,1,3}、跨组={1,3}；self 都为空。分组为 PE0–3、4–7、8–11、12–15。

**D=0 不在此次 4608 条中。** 本配置模式如果某个非 self 能力为零会拒绝构造成功查询 fixture；这不是调用真实 init 并验证 -100。D=0、RDMA backend 资源失败、UDMA 路由、CMO/quiet 数据面都需要另外的测试链路，不能由本次通过推导。

旧 internal_transport_mask 模式仍保留，用于任意缓存位转换/兼容回归。它可以人为注入不符合 950 实际 D2D 能力的 SDMA 位，因此旧结果 0x3/0x7 不作为本轮 950 配置测试结论。

## 11. 复现命令

从仓库根目录执行。当前 fixture 使用本机绝对路径；搬迁目录后先调整 root_info 与模拟 RootInfo 中的 topo_file_path。依赖 nlohmann/json 3.11.3、clang++ 与 Python 标准库，不需要 GoogleTest 或 NPU。

```bash
cd /Users/Admin/Projects/shmem-rfc

# 完整96场景；log是已有报告的输入。重定向会替换旧日志，需留档可先换一个文件名。
python3 -B tests/topology/cpp_json_stub/run.py \
  --json /private/tmp/shmem-rfc-json-v3.11.3 \
  --fixtures tests/topology/results/950-20261016/950-capability-cases.json \
  > tests/topology/results/950-20261016/950-capability-cpp.log 2>&1

# 快速检查一条配置驱动链
python3 -B tests/topology/cpp_json_stub/run.py \
  --fixtures tests/topology/results/950-20261016/950-capability-cases.json \
  --case biaoka_MTE_SDMA_source_0

# 基于日志再生成结果和Excel；不会重新执行C++
python3 tests/topology/results/950-20261016/generate_950_capability_report.py
python3 tests/topology/results/950-20261016/export_950_excel.py
```

后处理脚本还依赖该结果目录内已保存的 query-results.json（物理 baseline）及 api-report.html（页面模板）；它们不是 C++ 查询算法的依赖。96 场景 fixture 和 UBX 模拟 RootInfo 已保存为输入文件，本 runner 不会自动从 UBX TopoInfo 生成 RootInfo。

结果入口：

- [C++ 原始日志](../results/950-20261016/950-capability-cpp.log)
- [机器可读结果 JSON](../results/950-20261016/950-capability-results.json)
- [完整字段 CSV](../results/950-20261016/950-capability-results.csv)
- [交互报告](../results/950-20261016/950-capability-report.html)
- [Excel](../results/950-20261016/950拓扑接口测试结果.xlsx)

## 12. 完整测试入口源码快照

以下是编写本文时的完整 run.py 和 main.cpp，供不来回跳转时阅读。实际执行以链接中的工作区源文件为准；后续修改代码时应同步更新本文快照。生产关键函数已在第6–8节逐项列出，拓扑 helper 的完整实现由第2节生产源链接提供。

<details>
<summary>展开完整 run.py：生产代码提取、头文件替身和编译命令</summary>


```python
#!/usr/bin/env python3
"""TEMP_TOPOLOGY_CPP_JSON_STUB: compile real C++ topology queries on macOS/Linux.
Python orchestrates compilation only. No Python topology implementation is used.
"""
import argparse
import json
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--json', type=Path, help='nlohmann/json checkout (single_include/nlohmann/json.hpp)')
parser.add_argument('--fixtures', type=Path, help='Expected-value test suite; overrides direct RootInfo mode')
parser.add_argument('--root-info', type=Path, default=HERE/'root_info-biaoka.json')
parser.add_argument('--topology', type=Path, help='Override the topology file referenced by RootInfo')
parser.add_argument('--source-pe', type=int, default=0)
parser.add_argument('--transport-mask', type=lambda value: int(value, 0), default=1,
                    help='Mock INTERNAL transport bits: MTE=1 ROCE=2 SDMA=4 UDMA=8')
parser.add_argument('--case', help='Run just one named JSON case')
args = parser.parse_args()
if args.json is None:
    candidates = [ROOT / '3rdparty/json', Path('/private/tmp/shmem-rfc-json-v3.11.3')]
    args.json = next((p for p in candidates if (p/'single_include/nlohmann/json.hpp').is_file()), None)
if args.json is None or not (args.json/'single_include/nlohmann/json.hpp').is_file():
    parser.error('Supply --json /path/to/nlohmann-json; no CANN or GoogleTest required')

def function(path, name):
    source = (ROOT / path).read_text()
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return '#line ' + str(source[:start].count('\n') + 1) + ' \"' + str(ROOT / path) + '\"\n' + source[start:end] + '\n'


with tempfile.TemporaryDirectory(prefix='shmem-rfc-tests-') as folder:
    temp = Path(folder)
    inc = temp / 'include'
    def stub(name, content):
        path = inc / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    stub('acl/acl_rt.h', '#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    stub('shmemi_host_common.h', '''#pragma once
#include "host/shmem_host_def.h"
#include <sstream>
#define SHM_LOG_ERROR(m) do { std::ostringstream s; s << m; } while (0)
#define SHM_LOG_WARN(m) SHM_LOG_ERROR(m)
#define SHM_LOG_INFO(m) SHM_LOG_ERROR(m)
#define SHM_LOG_DEBUG(m) SHM_LOG_ERROR(m)
''')
    stub('shmemi_file_util.h', '''#pragma once
#include <filesystem>
#include <string>
namespace shm::utils { struct FileUtil {
static bool IsSymlink(const std::string& p) { return std::filesystem::is_symlink(p); }
static bool Realpath(std::string& p) {
    std::error_code error;
    const auto resolved = std::filesystem::canonical(p, error);
    if (error) return false;
    p = resolved.string();
    return true;
}
static bool IsFile(const std::string& p) { return std::filesystem::is_regular_file(p); }
static bool CheckFileSize(const std::string& p) { return std::filesystem::file_size(p) < 10*1024*1024; }
}; }
''')
    stub('dl_acl_api.h', '''#pragma once
#include <cstdint>
namespace shm { struct DlAclApi {
static int RtGetLogicDevIdByUserDevId(int32_t user, int32_t* logic) { *logic=user; return 0; }
static int AclrtGetPhyDevIdByLogicDevId(int32_t user, int32_t* phy) { *phy=user; return 0; }
}; }
''')
    # Preserve production public headers and bootstrap handle layout.
    stub('transport_def.h', '''#pragma once
namespace shm::transport { struct UdmaQpConfig {}; }
''')
    stub('init/shmemi_init.h', '''#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "host/init/shmem_host_init.h"
#include "utils/shmemi_host_types.h"
#include "transport/topo/topo_reader.h"
#include "transport_def.h"
struct entity_member {
    aclshmem_device_host_state_t* entity_host_state{};
    aclshmemi_bootstrap_handle_t* entity_boot_handle{};
    std::unique_ptr<shm::transport::TopologyContext> topology_context;
    std::vector<std::vector<aclshmemx_net_layer_t>> topology_net_layers_by_pe;
};
class aclshmemi_init_backend {
public:
    std::mutex entity_map_mutex_;
    std::map<uint64_t, entity_member*> entity_map_;
    int query_topology(uint64_t, int, uint32_t, aclshmemx_topology_t*);
    int build_topology_state(uint64_t);
    int bind_aclshmem_entity(aclshmemx_init_attr_t* attr, aclshmem_device_host_state_t* state,
        aclshmemi_bootstrap_handle_t* boot, std::nullptr_t,
        const shm::transport::UdmaQpConfig&, uint32_t, uint32_t) {
        auto* e = new entity_member;
        e->entity_host_state = state;
        e->entity_boot_handle = boot;
        entity_map_[attr->instance_id] = e;
        return 0;
    }
    int release_aclshmem_entity(uint64_t id) { delete entity_map_.at(id); entity_map_.erase(id); return 0; }
};
extern aclshmem_device_host_state_t g_state;
extern aclshmem_instance_ctx* g_instance_ctx;
extern aclshmemi_init_backend* init_manager;
''')
    production = '#include "init/shmemi_init.h"\n#include <cstddef>\n'
    production += 'aclshmem_device_host_state_t g_state{};\naclshmem_instance_ctx* g_instance_ctx{};\n'
    production += 'aclshmemi_init_backend* init_manager{};\nstatic std::mutex g_aclshmem_ctx_mutex;\n'
    src = (ROOT / 'src/host/init/shmem_init.cpp').read_text()
    start = src.index('namespace {\nconstexpr uint32_t TOPOLOGY_V1_SIZE')
    end = src.index('int32_t aclshmemx_init_attr(', start)
    production += '#line ' + str(src[:start].count('\n') + 1) + ' \"' + str(ROOT / 'src/host/init/shmem_init.cpp') + '\"\n' + src[start:end]
    for name in ['query_topology', 'build_topology_state']:
        production += function('src/host/init/backends/shmem_init_backend.cpp',
                               'int aclshmemi_init_backend::' + name + '(')
    (temp / 'queries.cpp').write_text(production)
    # Optional configuration-driven mode: execute production capability logic
    # rather than inject an already-computed topo_list. Memory mapping and QP
    # resources are still explicit test doubles, not inferred from LINKS.
    capability = '''#pragma once
#include "mem/heap/hybm_def.h"
#include "init/shmemi_init.h"
namespace shm {
struct RouteStub {
    int GetQpNetLayer(data_op_engine_type_t, uint32_t, uint32_t, uint32_t&) const {
        return ACLSHMEM_NOT_SUPPORTED;
    }
};
struct MemEntityDefault {
    hybm_options options_{};
    RouteStub* transportManager_ = nullptr;
    bool direct = false;
    bool SdmaReaches(uint32_t) const { return direct; }
    hybm_data_op_type CanReachDataOperators(uint32_t) const noexcept;
};
'''
    capability += function('src/host/entity/mem_entity_default.cpp',
                           'hybm_data_op_type MemEntityDefault::CanReachDataOperators(')
    capability += '}\ninline hybm_options ConfigureEngines(aclshmemx_init_attr_t* attributes) {\n'
    backend = (ROOT/'src/host/init/backends/shmem_init_backend.cpp').read_text()
    begin = backend.index('    hybm_options options{};', backend.index('::create_entity('))
    end = backend.index('    options.bmScope', begin)
    capability += backend[begin:end] + '    return options;\n}\n'
    capability += '''inline void StoreCapability(aclshmem_device_host_state_t* host_state,
        int32_t i, hybm_data_op_type reaches_types) {
'''
    begin = backend.index('        if (reaches_types & HYBM_DOP_TYPE_MTE)', backend.index('::reach_info_init('))
    end = backend.index('        // SDMA completion', begin)
    capability += backend[begin:end] + '}\n'
    stub('capability_fixture.h', capability)
    include_dirs = [inc, ROOT/'include', ROOT/'src/host',
                    ROOT/'src/host/bootstrap/config_store', ROOT/'src/host/transport',
                    args.json.resolve()/'single_include']
    flags = ['-std=c++17', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-missing-field-initializers', '-fsanitize=address,undefined', '-pthread',
             '-DACLSHMEM_SOC_950',
             '-DACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO="' + str(temp/'rootinfo.json') + '"',
             *['-I'+str(p) for p in include_dirs]]
    sources = [temp/'queries.cpp', HERE/'main.cpp',
               ROOT/'src/host/transport/topo/topo_reader.cpp',
               ROOT/'src/host/transport/topo/topology_context.cpp']
    binary = temp/'cpp-topology-json-stub'
    print('Compiling production C++ parser/context/query/build with ASan+UBSan...', flush=True)
    subprocess.run([os.environ.get('CXX', 'clang++'), *flags, *map(str, sources), '-o', str(binary)], check=True)
    if args.fixtures is not None or args.case:
        fixture_path = args.fixtures.resolve() if args.fixtures else HERE/'cases.json'
    else:
        spec = dict(name=args.root_info.stem, root_info=str(args.root_info.resolve()),
                    my_pe=args.source_pe, internal_transport_mask=args.transport_mask)
        if args.topology is not None:
            spec['topology'] = str(args.topology.resolve())
        fixture_path = temp/'input-case.json'
        fixture_path.write_text(json.dumps(dict(cases=[spec])))
    command = [str(binary), str(fixture_path)]
    if args.case:
        command.append(args.case)
    subprocess.run(command, check=True)
```

</details>

<details>
<summary>展开完整 main.cpp：runtime/bootstrap 桩、fixture、执行链、断言与日志</summary>

```cpp
// TEMP_TOPOLOGY_CPP_JSON_STUB: portable fixture, not a production initialization entry.
// All topology parsing/matching, context discovery, snapshot build and queries are
// compiled from production C++ sources; only runtime/bootstrap/resource setup is stubbed.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include "init/shmemi_init.h"
#include "capability_fixture.h"

using Json = nlohmann::json;
using namespace shm::transport;
namespace fs = std::filesystem;

static int driverFallbackCalls = 0;
extern "C" int topo_addr_info_get_size(int, size_t*) { ++driverFallbackCalls; return -1; }
extern "C" int topo_addr_info_get(int, char*, size_t*) { ++driverFallbackCalls; return -1; }

void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
Json Read(const fs::path& path)
{
    std::ifstream input(path);
    Require(input.good(), "Cannot open " + path.string());
    Json data;
    input >> data;
    return data;
}

struct Replay {
    std::vector<uint32_t> counts, ids, lengths;
    std::vector<std::vector<uint8_t>> blobs;
    int calls = 0;
    uint32_t source = 0;

    static int Gather(const void* send, void* receive, int bytes, aclshmemi_bootstrap_handle_t* handle)
    {
        auto& replay = *static_cast<Replay*>(handle->bootstrap_state);
        const int phase = ++replay.calls;
        // Production Initialize: agree/count/local-ID/length/agree/blob/agree.
        // Production build_topology_state: agree (phase 8).
        if (phase == 1 || phase == 5 || phase == 7 || phase == 8) {
            if (bytes != sizeof(int)) return -1;
            for (int pe = 0; pe < handle->npes; ++pe) {
                static_cast<int*>(receive)[pe] = *static_cast<const int*>(send);
            }
            return 0;
        }
        const std::vector<uint32_t>* values = nullptr;
        if (phase == 2) values = &replay.counts;
        if (phase == 3) values = &replay.ids;
        if (phase == 4) values = &replay.lengths;
        if (values) {
            if (bytes != sizeof(uint32_t) ||
                *static_cast<const uint32_t*>(send) != values->at(replay.source)) return -1;
            std::memcpy(receive, values->data(), values->size() * sizeof(uint32_t));
            return 0;
        }
        if (phase == 6) {
            const auto& own = replay.blobs.at(replay.source);
            if (bytes < 0 || static_cast<size_t>(bytes) < own.size() ||
                std::memcmp(send, own.data(), own.size()) != 0) return -1;
            std::memset(receive, 0, static_cast<size_t>(bytes) * replay.blobs.size());
            for (size_t pe = 0; pe < replay.blobs.size(); ++pe) {
                if (replay.blobs[pe].size() > static_cast<size_t>(bytes)) return -1;
                std::memcpy(static_cast<char*>(receive) + pe * bytes,
                            replay.blobs[pe].data(), replay.blobs[pe].size());
            }
            return 0;
        }
        return -1;
    }
};

// Construct the data that remote PEs would send, using the real C++ parsers and
// BuildLocalEndpoints. Never read expected_layers to create replay payloads.
Replay PreparePeers(const std::vector<uint32_t>& devices, uint32_t source)
{
    Replay replay;
    replay.source = source;
    for (const auto device : devices) {
        RootInfo root;
        TopoInfo topo;
        uint32_t id = 0, count = 0;
        Require(TopoReader::ParseRootInfo(device, root), "Production ParseRootInfo failed");
        Require(TopoReader::ParseTopoInfo(root.topoFilePath, topo), "Production ParseTopoInfo failed");
        Require(TopoReader::GetLocalId(root, device, id), "GetLocalId failed");
        Require(TopoReader::GetEidCount(root, count), "GetEidCount failed");
        replay.ids.push_back(id);
        replay.counts.push_back(count);
        auto blob = Json::to_msgpack(Json(TopologyContext::BuildLocalEndpoints(root, topo)));
        replay.lengths.push_back(static_cast<uint32_t>(blob.size()));
        replay.blobs.push_back(std::move(blob));
    }
    return replay;
}

struct Fixture {
    aclshmemi_init_backend backend;
    aclshmem_device_host_state_t state{};
    aclshmem_instance_ctx instance{23, nullptr};
    aclshmemx_init_attr_t attr{};
    aclshmemi_bootstrap_handle_t boot{};
    bool bound = false;
    ~Fixture()
    {
        if (bound) backend.release_aclshmem_entity(instance.id);
        init_manager = nullptr;
        g_instance_ctx = nullptr;
        g_state = {};
    }
};

void RunCase(const Json& spec, const fs::path& directory)
{
    const std::string name = spec.at("name");
    const auto rootPath = directory / spec.at("root_info").get<std::string>();
    auto input = Read(rootPath);
    // Resolve the referenced topology file locally; never invent physical edges.
    fs::path topologyPath;
    if (spec.contains("topology")) {
        topologyPath = directory / spec.at("topology").get<std::string>();
    } else {
        const fs::path described = input.at("topo_file_path").get<std::string>();
        topologyPath = described.is_absolute() ? described : rootPath.parent_path() / described;
        if (!fs::is_regular_file(topologyPath)) {
            topologyPath = rootPath.parent_path() / described.filename();
        }
    }
    Require(fs::is_regular_file(topologyPath),
            "Topology file missing: " + topologyPath.string() + "; supply --topology");
    input["topo_file_path"] = fs::canonical(topologyPath).string();
    {
        std::ofstream output(TopoReader::ROOTINFO_PATH);
        Require(output.good(), "Cannot create isolated RootInfo fixture");
        output << input.dump(2);
    }
    std::vector<uint32_t> devices;
    if (spec.contains("devices")) {
        devices = spec.at("devices").get<std::vector<uint32_t>>();
    } else {
        for (const auto& rank : input.at("rank_list")) {
            devices.push_back(rank.at("device_id").get<uint32_t>());
        }
        if (input.contains("rank_count")) {
            Require(input.at("rank_count").get<size_t>() == devices.size(), "rank_count mismatch");
        }
    }
    Require(std::set<uint32_t>(devices.begin(), devices.end()).size() == devices.size(),
            "Duplicate device IDs in RootInfo");
    const auto source = spec.value("my_pe", 0U);
    for (size_t pe = 0; pe < devices.size(); ++pe) {
        std::cout << "[DEVICE] PE " << pe << " -> device_id " << devices[pe] << '\n';
    }
    Require(!devices.empty() && devices.size() <= ACLSHMEM_MAX_PES && source < devices.size(),
            "Invalid PE count or source");
    auto replay = PreparePeers(devices, source);
    Fixture fixture;
    fixture.boot.npes = static_cast<int>(devices.size());
    fixture.boot.mype = static_cast<int>(source);
    fixture.boot.bootstrap_state = &replay;
    fixture.boot.allgather = Replay::Gather;

    // REAL Initialize: calls mocked device-ID conversion, real ParseRootInfo/
    // ParseTopoInfo, real MessagePack encoding/decoding, and replay allgather.
    auto context = std::make_unique<TopologyContext>();
    Require(context->Initialize(static_cast<int32_t>(devices[source]), source,
                                static_cast<uint32_t>(devices.size()), &fixture.boot) == ACLSHMEM_SUCCESS,
            name + ": production TopologyContext::Initialize failed");
    Require(replay.calls == 7, "Unexpected discovery exchange order");
    Require(context->rank_to_local_id == replay.ids, "Remote local-ID exchange mismatch");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        Require(Json(context->rank_to_sync_endpoints[peer]) == Json::from_msgpack(replay.blobs[peer]),
                "Remote endpoint exchange mismatch");
    }

    fixture.attr.instance_id = fixture.instance.id;
    fixture.attr.n_pes = fixture.state.npes = fixture.boot.npes;
    fixture.attr.my_pe = fixture.state.mype = fixture.boot.mype;
    Require(fixture.backend.bind_aclshmem_entity(&fixture.attr, &fixture.state, &fixture.boot,
                nullptr, UdmaQpConfig{}, 1, 1) == 0, "Fixture bind failed");
    fixture.bound = true;
    fixture.backend.entity_map_.at(fixture.instance.id)->topology_context = std::move(context);
    const bool configurationMode = spec.contains("engine_config");
    Require(!configurationMode || (!spec.contains("internal_transport_mask") &&
            !spec.contains("internal_transport_masks")), "Cannot combine configuration and raw state injection");
    if (configurationMode) {
        const auto config = spec.at("engine_config").get<uint32_t>();
        Require((config & ~7U) == 0, "Configuration fixture currently covers MTE/SDMA/ROCE only");
        fixture.attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(config);
        const auto direct = spec.at("direct_reachable").get<std::vector<bool>>();
        Require(direct.size() == devices.size() && direct[source], "Explicit D per peer and reachable self required");
        shm::MemEntityDefault entity;
        entity.options_ = ConfigureEngines(&fixture.attr);
        entity.options_.rankId = source;
        for (size_t peer = 0; peer < devices.size(); ++peer) {
            entity.direct = direct[peer];
            const auto capability = entity.CanReachDataOperators(static_cast<uint32_t>(peer));
            Require(peer == source || capability != 0, "No reachable remote engine: no successful-init query fixture");
            StoreCapability(&fixture.state, static_cast<int32_t>(peer), capability);
        }
    }
    const auto masks = spec.contains("internal_transport_masks")
        ? spec.at("internal_transport_masks").get<std::vector<uint32_t>>()
        : std::vector<uint32_t>(devices.size(), spec.value("internal_transport_mask", 1U));
    Require(masks.size() == devices.size(), "Wrong mask count");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        Require(masks[peer] <= 15, "Unknown internal transport bits");
        if (!configurationMode) fixture.state.topo_list[peer] = static_cast<aclshmem_transport_t>(masks[peer]);
    }
    init_manager = &fixture.backend;
    g_instance_ctx = &fixture.instance;
    g_state.npes = fixture.state.npes;
    g_state.mype = fixture.state.mype;

    // REAL production build, not a precomputed layer injection.
    Require(fixture.backend.build_topology_state(fixture.instance.id) == ACLSHMEM_SUCCESS,
            "Production build_topology_state failed");
    Require(replay.calls == 8, "Missing build status agreement");
    aclshmemx_topology_t validation = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_NOT_INITED,
            "Query must reject uninitialized instance");
    g_state.is_aclshmem_initialized = true;
    Require(aclshmemx_query_topology(0, 3, nullptr) == ACLSHMEM_INVALID_PARAM, "Null output accepted");
    Require(aclshmemx_query_topology(-1, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Bad PE accepted");
    Require(aclshmemx_query_topology(0, 0, &validation) == ACLSHMEM_INVALID_VALUE, "Zero mask accepted");
    validation.struct_size = 63;
    Require(aclshmemx_query_topology(0, 3, &validation) == ACLSHMEM_INVALID_VALUE, "Short ABI accepted");

    const bool checkEngines = spec.contains("expected_engine_masks");
    const bool checkLayers = spec.contains("expected_layers");
    const auto expectedEngines = checkEngines
        ? spec.at("expected_engine_masks").get<std::vector<uint32_t>>() : std::vector<uint32_t>{};
    const auto expectedLayers = checkLayers
        ? spec.at("expected_layers").get<std::vector<std::vector<uint32_t>>>()
        : std::vector<std::vector<uint32_t>>{};
    Require((!checkEngines || expectedEngines.size() == devices.size()) &&
            (!checkLayers || expectedLayers.size() == devices.size()), "Wrong expectation count");
    for (size_t peer = 0; peer < devices.size(); ++peer) {
        for (uint32_t mask : {1U, 2U, 3U}) {
            aclshmemx_topology_t output = ACLSHMEMX_TOPOLOGY_INITIALIZER;
            output.reserved[0] = 0x12345678U;
            // REAL public C++ function body from shmem_init.cpp.
            const int status = aclshmemx_query_topology(static_cast<int>(peer), mask, &output);
            Require(status == ACLSHMEM_SUCCESS, "Public query failed: " + std::to_string(status));
            std::vector<uint32_t> layers;
            for (uint32_t i = 0; i < output.net_layer_count; ++i) {
                layers.push_back(static_cast<uint32_t>(output.net_layers[i]));
            }
            const uint32_t expectedEngine = mask & 1U && checkEngines ? expectedEngines[peer] : 0U;
            const auto expectedLayer = mask & 2U && checkLayers ? expectedLayers[peer] : std::vector<uint32_t>{};
            Require((!checkEngines && (mask & 1U)) || output.engine_mask == expectedEngine, name + ": ENGINES mismatch at PE " + std::to_string(peer) +
                    " actual=" + std::to_string(output.engine_mask) + " expected=" + std::to_string(expectedEngine));
            Require((!checkLayers && (mask & 2U)) || layers == expectedLayer, name + ": LINKS mismatch at PE " + std::to_string(peer) +
                    " actual=" + Json(layers).dump() + " expected=" + Json(expectedLayer).dump());
            Require(output.reserved[0] == 0x12345678U, "Query changed reserved field");
            Require(!layers.empty() || output.net_layers == nullptr, "Empty layers must use nullptr");
            std::cout << Json{{"mode", "production_cpp"}, {"case", name}, {"source_pe", source},
                              {"engine_state_source", configurationMode ? "production_capability_with_stub_D" : "raw_transport_injection"},
                              {"engine_config", configurationMode ? spec.at("engine_config") : Json(nullptr)},
                              {"target_pe", peer}, {"target_device_id", devices[peer]},
                              {"source_device_id", devices[source]},
                              {"engine_expectation_checked", checkEngines}, {"links_expectation_checked", checkLayers},
                              {"query_mask", mask}, {"status", status},
                              {"engine_mask", output.engine_mask}, {"net_layer_count", output.net_layer_count},
                              {"net_layers", layers}}.dump() << '\n';
        }
    }
    Require(driverFallbackCalls == 0, "Unexpected hardware rootinfo fallback");
    std::cout << "[CHECK] " << name << ": engines expected=" << checkEngines
              << ", links expected=" << checkLayers << " (0 = observation only)\n";
}

int main(int argc, char** argv)
{
    try {
        Require(argc == 2 || argc == 3, "Usage: cpp-stub cases.json [case-name]");
        const fs::path path = fs::absolute(argv[1]);
        const auto suite = Read(path);
        int passed = 0;
        for (const auto& spec : suite.at("cases")) {
            if (argc == 3 && spec.at("name").get<std::string>() != argv[2]) continue;
            RunCase(spec, path.parent_path());
            ++passed;
        }
        Require(passed > 0, "No matching cases");
        std::cout << "PASS: " << passed << " C++ JSON cases (real public query, ASan+UBSan)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
```

</details>
