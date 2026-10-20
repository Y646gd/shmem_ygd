# RFC 需求—实现代码—UT 断言—实际结果

本说明面向实现审查，不以“通过多少项”代替需求证明。每个主要功能按：**RFC 要求 → 当前怎么实现 → UT 怎样断言 → 本轮实际证据 → 剩余边界** 展开。

- 需求：[RFC.md](/Users/Admin/Projects/RFC.md)，以第9～11节补充为准。
- 实现：`/Users/Admin/Projects/shmem1019`，基线 `a23c173` 加工作区增量。
- 执行证据：[本轮完整日志、XML及指纹](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/README.md)。生成本文前，已逐项核验本轮源码指纹仍与当前源文件一致。

**UT 是可执行的测试代码；实现说明是审查材料，两者应相互对应。** 本文同时给出正式库实现、测试断言和结果，不把测试桩当成真实设备运行。

证据分三类：直接编译真实 parser/context/router；提取正式 query/能力/初始化控制函数体并替换依赖；源码接线检查。对应边界逐节说明。

## 1. RFC 第1章：公开查询入口、返回字段和生命周期

### 1.1 要求与实现路径

```text
应用调用 query_topology
 → 校验参数/ABI/初始化状态/目标PE
 → 用当前实例ID查找 entity_member
 → 转换 topo_list 为公开 engine_mask
 → 返回实例网络层缓存的只读指针
```

### 1.2 实现代码：公开校验

**代码位置：** [src/host/init/shmem_init.cpp:1381](/Users/Admin/Projects/shmem1019/src/host/init/shmem_init.cpp:1381)

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

它先拒绝空地址、未知查询位、短结构体和错误版本，再确认当前实例已初始化及PE有效。查询读取当前激活实例；不是按物理设备ID直接查询。

### 1.3 实现代码：装配结果

**代码位置：** [src/host/init/backends/shmem_init_backend.cpp:197](/Users/Admin/Projects/shmem1019/src/host/init/backends/shmem_init_backend.cpp:197)

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

内部ROCE/SDMA位为2/4，公开位为4/2，因此逐位映射。仅查询LINKS时引擎字段为0；仅查询ENGINES时网络层字段清零。结构体输入头和reserved不写入。

### 1.4 UT代码：不是只判断返回成功

**代码位置：** [tests/unittest/host/init/topology_query_test.cpp:81](/Users/Admin/Projects/shmem1019/tests/unittest/host/init/topology_query_test.cpp:81)

```cpp
TEST_F(TopologyQueryTest, ConvertsEveryInternalBitAndPreservesCallerLayout)
{
    struct Extended {
        aclshmemx_topology_t value = ACLSHMEMX_TOPOLOGY_INITIALIZER;
        uint64_t tail = 0x123456789ULL;
    } out;
    out.value.struct_size = sizeof(out);
    out.value.reserved[3] = 0xabcdef;
    EXPECT_EQ(sizeof(aclshmemx_topology_t), 64U);
    EXPECT_EQ(offsetof(aclshmemx_topology_t, reserved), 24U);
    const uint32_t expected[] = {ACLSHMEM_DATA_OP_MTE, ACLSHMEM_DATA_OP_ROCE,
                                ACLSHMEM_DATA_OP_SDMA, ACLSHMEM_DATA_OP_UDMA};
    for (uint32_t bits = 0; bits < 16; ++bits) {
        state.topo_list[1] = static_cast<aclshmem_transport_t>(bits);
        uint32_t mask = 0;
        for (uint32_t bit = 0; bit < 4; ++bit) if (bits & (1U << bit)) mask |= expected[bit];
        ASSERT_EQ(aclshmemx_query_topology(1, ACLSHMEMX_TOPOLOGY_QUERY_ENGINES, &out.value), ACLSHMEM_SUCCESS);
        EXPECT_EQ(out.value.engine_mask, mask);
        EXPECT_EQ(out.value.net_layer_count, 0U);
        EXPECT_EQ(out.value.net_layers, nullptr);
        EXPECT_EQ(out.value.struct_size, sizeof(out));
        EXPECT_EQ(out.value.abi_version, ACLSHMEMX_TOPOLOGY_ABI_VERSION);
        EXPECT_EQ(out.value.reserved[3], 0xabcdefU);
        EXPECT_EQ(out.tail, 0x123456789ULL);
    }
}
```

该UT遍历内部0～15所有位组合，检查每个公开位、输出清零、输入头、reserved和额外尾部哨兵。补充UT还检查全部五个reserved和所有未定义查询位：

**代码位置：** [tests/topology/rfc_requirements_test.cpp:128](/Users/Admin/Projects/shmem1019/tests/topology/rfc_requirements_test.cpp:128)

```cpp
TEST_F(RfcContractTest, Rfc1AllMasksPreserveAllReservedAndExtendedTail) {
    auto& e = *backend.entity_map_.at(81);
    e.topology_context = RoceContext(1);
    ASSERT_EQ(backend.build_topology_state(81), 0);
    struct Extended { aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER; std::array<uint64_t, 4> tail; } out;
    for (uint32_t mask : {1U, 2U, 3U}) for (int peer : {0, 1}) {
        out.v.struct_size = sizeof(out); out.v.abi_version = 1;
        for (uint32_t i = 0; i < 5; ++i) out.v.reserved[i] = 100 + i;
        out.tail.fill(0xcafef00dULL);
        // Poison previous outputs so unrequested fields must actively be cleared.
        out.v.engine_mask = 0xffffffffU; out.v.net_layer_count = 77;
        ASSERT_EQ(aclshmemx_query_topology(peer, mask, &out.v), 0);
        EXPECT_EQ(out.v.struct_size, sizeof(out)); EXPECT_EQ(out.v.abi_version, 1U);
        for (uint32_t i = 0; i < 5; ++i) EXPECT_EQ(out.v.reserved[i], 100U + i);
        for (auto t : out.tail) EXPECT_EQ(t, 0xcafef00dULL);
        EXPECT_EQ(out.v.engine_mask, (mask & 1U) ? (peer == 0 ? 4U : 1U) : 0U);
        const bool has_layer = (mask & 2U) && peer != 1;
        EXPECT_EQ(out.v.net_layer_count, has_layer ? 1U : 0U);
        if (has_layer) { ASSERT_NE(out.v.net_layers, nullptr); EXPECT_EQ(out.v.net_layers[0], ACLSHMEM_NET_LAYER_3); }
        else EXPECT_EQ(out.v.net_layers, nullptr);
    }
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] TopologyQueryTest.ConvertsEveryInternalBitAndPreservesCallerLayout
[PASS] RfcContractTest.Rfc1AllMasksPreserveAllReservedAndExtendedTail
[PASS] RfcContractTest.Rfc1AllInvalidBitsSizesVersionsAndMissingActiveState
[PASS] RfcContractTest.Rfc1CurrentInstanceWorldPeAndQueryPerformsNoCollectives
```

结果含义：上述具体断言执行通过。XML只保留测试级PASS和时间，没有逐个assert的数值日志；这里不伪造一份独立“实际值打印”。逐值查询输出见第9节的原始日志摘录。

### 1.5 Python实现与UT

高层实现：[topology.py:19](/Users/Admin/Projects/shmem1019/src/python/shmem/core/topology.py:19)；native绑定：[pyshmem.cpp:510](/Users/Admin/Projects/shmem1019/src/host/python_wrapper/pyshmem.cpp:510)。

高层负责int32/bool校验、mask、实例锁和异常；native负责调用C++、复制列表并返回(status,payload)。

**代码位置：** [tests/topology/test_python_topology.py:82](/Users/Admin/Projects/shmem1019/tests/topology/test_python_topology.py:82)

```python
    def test_instance_lock_blocks_other_thread_during_native_call_and_releases_on_error(self):
        acquired = []
        def native(*_):
            def competing_finalize():
                ok = self.utils._instance_lock.acquire(blocking=False)
                acquired.append(ok)
                if ok:
                    self.utils._instance_lock.release()
            thread = threading.Thread(target=competing_finalize)
            thread.start(); thread.join(timeout=2)
            self.assertFalse(thread.is_alive())
            return -5, None
        self.native.aclshmemx_query_topology = native
        with self.assertRaises(self.utils.AclshmemError):
            self.module.query_topology(1)
        self.assertEqual(acquired, [False])
        self.assertFalse(self.utils._instance_lock._is_owned())
```

本轮：[python.log](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/python.log)，7项通过。这里运行真实高层wrapper，但native是桩；真实pybind模块的编译链接和复制行为没有动态执行，只有源码检查，不能宣布已通过。

## 2. RFC 第2章：从拓扑结果落实到实际QP与分段

### 2.1 当前实现

1019已有QP配置/getter、分段、kernel与完成同步。新增的VerifyTwoLayerPlan在传输前验证：有UDMA、物理有0/1两层、实际QP0/1分别属于0/1。

**代码位置：** [examples/topology_query/topology_plan.h:13](/Users/Admin/Projects/shmem1019/examples/topology_query/topology_plan.h:13)

```cpp
inline int VerifyTwoLayerPlan(int peer)
{
    aclshmemx_topology_t info = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    int status = aclshmemx_query_topology(peer,
        ACLSHMEMX_TOPOLOGY_QUERY_ENGINES | ACLSHMEMX_TOPOLOGY_QUERY_LINKS, &info);
    if (status != ACLSHMEM_SUCCESS) return status;
    if (!(info.engine_mask & ACLSHMEM_DATA_OP_UDMA)) return ACLSHMEM_NOT_SUPPORTED;
    bool found[2] = {false, false};
    for (uint32_t i = 0; i < info.net_layer_count; ++i) {
        const auto layer = static_cast<uint32_t>(info.net_layers[i]);
        if (layer < 2) found[layer] = true;
    }
    if (!found[0] || !found[1]) return ACLSHMEM_NOT_SUPPORTED;
    uint32_t count = 0;
    status = aclshmemx_get_qp_num(ACLSHMEM_DATA_OP_UDMA, &count);
    if (status != ACLSHMEM_SUCCESS) return status;
    if (count != 2) return ACLSHMEM_INVALID_VALUE;
    for (uint32_t qp = 0; qp < 2; ++qp) {
        aclshmemx_net_layer_t layer = ACLSHMEM_NET_LAYER_DEFAULT;
        status = aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, peer, qp, &layer);
        if (status != ACLSHMEM_SUCCESS) return status;
        if (static_cast<uint32_t>(layer) != qp) return ACLSHMEM_NOT_SUPPORTED;
    }
    std::cout << "[TOPOLOGY] peer=" << peer << " engines=" << info.engine_mask
              << " layers include 0,1; verified actual QP 0 -> layer 0, QP 1 -> layer 1\n";
    return ACLSHMEM_SUCCESS;
}
```

### 2.2 UT：物理有双层也不能直接当作资源可用

**代码位置：** [tests/topology/planner_test.cpp:54](/Users/Admin/Projects/shmem1019/tests/topology/planner_test.cpp:54)

```cpp
TEST_F(RfcPlannerTest, Rfc2TwoQpsOnSameLayerDoNotProveTwoLayerExecution) {
    resource.routes = {ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_0};
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED);
    resource.routes = {ACLSHMEM_NET_LAYER_1, ACLSHMEM_NET_LAYER_0};
    EXPECT_EQ(VerifyTwoLayerPlan(7), ACLSHMEM_NOT_SUPPORTED); // Current sample fixes the QP mapping.
    resource.routes = {ACLSHMEM_NET_LAYER_0, ACLSHMEM_NET_LAYER_1};
    EXPECT_EQ(VerifyTwoLayerPlan(7), 0);
}
```

此测试直接包含实际topology_plan.h；只替换公开资源查询的返回值。先制造两个QP都在layer0，再制造映射反转，最后给出正确映射，分别断言拒绝、拒绝、成功。

### 2.3 UT：数据完整覆盖且Host/kernel公式一致

**代码位置：** [tests/topology/planner_test.cpp:85](/Users/Admin/Projects/shmem1019/tests/topology/planner_test.cpp:85)

```cpp
TEST(RfcSlicesTest, Rfc2HostAndKernelCoverAllElementsWithoutOverlap) {
    std::vector<uint64_t> lengths{0, 1, 2, 3, 8, 9, 1023, 1024, 1025, UINT64_MAX};
    for (uint64_t total : lengths) for (uint32_t qps : {1U, 2U, 4U}) {
        SCOPED_TRACE(testing::Message() << "N=" << total << " qps=" << qps);
        const auto slices = BuildQpSlices(total, qps);
        uint64_t cursor = 0, minimum = UINT64_MAX, maximum = 0;
        ASSERT_EQ(slices.size(), qps);
        for (uint32_t qp = 0; qp < qps; ++qp) {
            const auto& slice = slices[qp];
            EXPECT_EQ(slice.offset, cursor); // Every interval starts where the previous one ends.
            ASSERT_LE(slice.length, total - cursor); cursor += slice.length;
            minimum = std::min(minimum, slice.length); maximum = std::max(maximum, slice.length);
            const auto device = KernelSliceForTest(total, qp, qps);
            EXPECT_EQ(device.offset, slice.offset); EXPECT_EQ(device.length, slice.length);
        }
        EXPECT_EQ(cursor, total); EXPECT_LE(maximum - minimum, 1U);
    }
    const auto odd = BuildQpSlices(9, 2);
    EXPECT_EQ(odd[0].length, 5U); EXPECT_EQ(odd[1].offset, 5U); EXPECT_EQ(odd[1].length, 4U);
}
```

测试实际BuildQpSlices函数和从kernel提取的原始分段算术；用0、奇偶、边界长度和1/2/4个QP检查连续无重叠、总长度完整、大小差不超过1，以及Host/kernel一致。没有启动NPU kernel。

**本轮执行记录：** [planner/planner.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/planner/planner.xml)

```text
[PASS] RfcPlannerTest.Rfc2RequiresUdmaAndBothPhysicalLayersBeforeResourceLookup
[PASS] RfcPlannerTest.Rfc2TwoQpsOnSameLayerDoNotProveTwoLayerExecution
[PASS] RfcPlannerTest.Rfc2SampleRejectsQpCountsOtherThanTwo
[PASS] RfcPlannerTest.Rfc2PropagatesQueryAndResourceErrors
[PASS] RfcPlannerTest.Rfc2ExtraRoceLayerDoesNotAffectValidTwoLayerResources
[PASS] RfcSlicesTest.Rfc2HostAndKernelCoverAllElementsWithoutOverlap
```

**未宣称完成：** 当前固定两个QP，不是RFC伪代码中的任意QP搜索规划器。9元素采用1019既有5+4分段，RFC示意为4+5。真实双层传输、数据完成和路径trace仍待设备验证。

## 3. RFC 第3章：物理层语义、端口/plane与RoCE域

### 3.1 实现：先匹配物理描述，再归并层

**代码位置：** [src/host/transport/topo/topo_reader.cpp:938](/Users/Admin/Projects/shmem1019/src/host/transport/topo/topo_reader.cpp:938)

```cpp
bool PhysicalEndpointPairMatches(
    const TopoInfo& topo, const SyncEndpoint& local, const SyncEndpoint& peer,
    const std::string& localPort, const std::string& peerPort, TopoFabric fabric, uint32_t rawLayer)
{
    auto matches = [&](const SyncEndpoint& endpoint, const std::string& port) {
        if (!EndpointHasPort(endpoint, port)) return false;
        if (endpoint.netType == NetType::Mesh) return fabric == TopoFabric::Mesh;
        if (endpoint.netType == NetType::Clos) return fabric == TopoFabric::Clos;
        return endpoint.netType == NetType::TopoFileDesc && PortMatchesFabric(topo, port, fabric);
    };
    if (!matches(local, localPort) || !matches(peer, peerPort)) return false;
    if (fabric == TopoFabric::Mesh) {
        return local.netLayer == rawLayer && peer.netLayer == rawLayer &&
               IsUsableNetInstanceId(local.netInstanceId) && local.netInstanceId == peer.netInstanceId;
    }
    if (local.planeId.empty() || local.planeId != peer.planeId) return false;
    if (local.netType == NetType::Clos || peer.netType == NetType::Clos) {
        return local.netLayer == rawLayer && peer.netLayer == rawLayer &&
               IsUsableNetInstanceId(local.netInstanceId) && local.netInstanceId == peer.netInstanceId;
    }
    // The caller has already matched the two attachments' topology instance and layer.
    return true;
}
```

Mesh检查端口、fabric、原始层和网络实例，不额外要求plane相等。Clos路径先要求相同非空plane；显式Clos再检查层与网络实例；两端TopoFileDesc依赖外层已匹配的附件层和topoInstanceId。

物理端口不是TCP端口；例如0/6表示Die0/Port6。SyncEndpoint是地址关联、层、网络实例、plane和端口的摘要，不是QP或硬件endpoint句柄。

### 3.2 实现：RoCE layer3由网络域声明判断

**代码位置：** [src/host/transport/topo/topo_reader.cpp:962](/Users/Admin/Projects/shmem1019/src/host/transport/topo/topo_reader.cpp:962)

```cpp
bool HasRoceLayerRelation(const std::vector<SyncEndpoint>& local, const std::vector<SyncEndpoint>& peer)
{
    for (const auto& a : local) {
        for (const auto& b : peer) {
            if (a.netLayer == 3 && b.netLayer == 3 && a.netType == NetType::Clos && b.netType == NetType::Clos &&
                IsUsableNetInstanceId(a.netInstanceId) && a.netInstanceId == b.netInstanceId &&
                !a.planeId.empty() && a.planeId == b.planeId && a.addrType == b.addrType &&
                (a.addrType == RankAddrType::IPV4 || a.addrType == RankAddrType::IPV6)) {
                return true;
            }
        }
    }
    return false;
}
```

双方必须是layer3 CLOS、同有效网络实例、同非空plane、同IPv4/IPv6地址族。函数不检查ROCE使能、不比较IP是否相等，也不发包探测。错误地把隔离网络标成同一cluster仍可能误描述为连通，这属于输入/部署验收边界。

### 3.3 UT：不仅测试正例，还破坏每项匹配条件

**代码位置：** [tests/unittest/host/topo/topology_layers_test.cpp:87](/Users/Admin/Projects/shmem1019/tests/unittest/host/topo/topology_layers_test.cpp:87)

```cpp
TEST_F(TopologyLayersTest, NetworkInstancePlaneAndAttachmentBoundaries)
{
    Add(NetType::Mesh, 0);
    Add(NetType::Clos, 1);
    endpoints[1][0].netInstanceId = "another-server";
    EXPECT_EQ(Query(), (std::vector<uint32_t>{1}));
    endpoints[1][1].planeId = "right";
    EXPECT_TRUE(Query().empty());
    endpoints[1][1].planeId = "left";
    topo.closTopoEdges[1].topoInstanceId = 43;
    EXPECT_TRUE(Query().empty());
    topo.closTopoEdges[1].topoInstanceId = 42;
    endpoints[1][1].netInstanceId = "other-domain";
    EXPECT_TRUE(Query().empty());
}
```

**代码位置：** [tests/unittest/host/topo/topology_layers_test.cpp:237](/Users/Admin/Projects/shmem1019/tests/unittest/host/topo/topology_layers_test.cpp:237)

```cpp
TEST_F(TopologyLayersTest, RoceFullFabricAllPairsWithRepeatedPhysicalIdsRemainDomainScoped)
{
    // Reused local IDs across servers must not collapse world-PE membership.
    std::vector<uint32_t> local_ids{0, 1, 0, 1};
    std::vector<std::vector<SyncEndpoint>> peers(4);
    for (uint32_t p = 0; p < 4; ++p) {
        SyncEndpoint ep{3, "fabric-A", NetType::Clos, TopoFabric::Clos, "plane", {"d2h"}, p};
        ep.addrType = RankAddrType::IPV6; peers[p].push_back(ep);
    }
    peers[3][0].netInstanceId = "fabric-B";
    for (uint32_t source = 0; source < 4; ++source) {
        TopoQuerier query(root, topo, source, local_ids, peers);
        for (uint32_t target = 0; target < 4; ++target) {
            std::vector<uint32_t> layers;
            query.GetReachableNetLayers(target, layers);
            const bool connected = source != target && source != 3 && target != 3;
            EXPECT_EQ(layers, connected ? std::vector<uint32_t>{3} : std::vector<uint32_t>{});
        }
    }
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] TopologyLayersTest.NetworkInstancePlaneAndAttachmentBoundaries
[PASS] TopologyLayersTest.AllEightPhysicalLayerSetsWithDuplicatePlanesAndPorts
[PASS] TopologyLayersTest.RejectsWrongPortFabricLayerAndUnknownMeshDomain
[PASS] TopologyLayersTest.AggregatedAndSeparatePortDescriptionsYieldSameLayer
[PASS] TopologyLayersTest.RoceRejectsDifferentNonemptyPlanesEmptyUnknownDomainsAndEid
[PASS] TopologyLayersTest.RoceFullFabricAllPairsWithRepeatedPhysicalIdsRemainDomainScoped
```

这些测试直接编译真实topo_reader.cpp，不是用Python重写拓扑算法。输入端点/边由fixture构造；真实IO DIE、RoCE全互通及完整Topo工具仍未被UT证明。

## 4. RFC 第4章：发现、共享context、缓存和释放

### 4.1 实现路径与位置

| 步骤 | 当前实现 |
| --- | --- |
| 创建并发现 | [create_entity](/Users/Admin/Projects/shmem1019/src/host/init/backends/shmem_init_backend.cpp:361)，DEVICE_SIDE创建context并调用Initialize |
| 解析/交换 | [Initialize](/Users/Admin/Projects/shmem1019/src/host/transport/topo/topology_context.cpp:133)，真实parser、MessagePack、显式bootstrap |
| UDMA借用或自建 | [OpenDevice](/Users/Admin/Projects/shmem1019/src/host/transport/device_udma/device_udma_transport_manager.cpp:285) |
| 层缓存 | [build_topology_state](/Users/Admin/Projects/shmem1019/src/host/init/backends/shmem_init_backend.cpp:252) |
| 查询 | 本文第1节，直接读缓存，不重新发现 |

### 4.2 实现代码：缓存构建与失败汇合

**代码位置：** [src/host/init/backends/shmem_init_backend.cpp:252](/Users/Admin/Projects/shmem1019/src/host/init/backends/shmem_init_backend.cpp:252)

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

固定当前源PE，逐目标求层，构建临时表后move到实例。异常转换成已有-8/-4，最后Agree汇合。缓存提交在Agree之前，因此远端失败时本地可能已有数据，初始化必须以最终状态停止发布。

### 4.3 UT：实际发现阶段失败不能跳过汇合

**代码位置：** [tests/topology/context_test.cpp:82](/Users/Admin/Projects/shmem1019/tests/topology/context_test.cpp:82)

```cpp
TEST_F(DiscoveryTest, EveryCollectiveFailureStopsFurtherExchanges)
{
    for (int phase = 1; phase <= 7; ++phase) {
        exchange = {};
        exchange.fail_at = phase;
        TopologyContext context;
        EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_BOOTSTRAP_ERROR) << phase;
        EXPECT_EQ(exchange.calls, phase);
    }
}
```

**代码位置：** [tests/topology/context_test.cpp:136](/Users/Admin/Projects/shmem1019/tests/topology/context_test.cpp:136)

```cpp
TEST_F(DiscoveryTest, Rfc4ZeroEidCountAndOversizedPeerBlobStopBeforeBlobExchange)
{
    TopologyContext context;
    exchange.zero_counts = true;
    EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(exchange.calls, 5);
    exchange = {}; exchange.huge_lengths = true;
    EXPECT_EQ(context.Initialize(0, 0, 2, &boot), ACLSHMEM_INVALID_VALUE);
    EXPECT_EQ(exchange.calls, 5);
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] DiscoveryTest.DiscoversIpOnlyTopologyWithoutCreatingUdmaResources
[PASS] DiscoveryTest.EveryCollectiveFailureStopsFurtherExchanges
[PASS] DiscoveryTest.RemoteAllocationFailurePreventsNextPayloadExchange
[PASS] DiscoveryTest.Rfc4RuntimeMappingFailuresAreAgreed
[PASS] DiscoveryTest.Rfc4ZeroEidCountAndOversizedPeerBlobStopBeforeBlobExchange
[PASS] TopologyJsonStubTest.ReadsJsonAndQueriesRealPublicApi
```

### 4.4 UT：快照、借用和释放

**代码位置：** [tests/topology/rfc_requirements_test.cpp:151](/Users/Admin/Projects/shmem1019/tests/topology/rfc_requirements_test.cpp:151)

```cpp
TEST_F(RfcContractTest, Rfc4SnapshotIsStableUntilFixtureOwnerRelease) {
    auto& e = *backend.entity_map_.at(81); e.topology_context = RoceContext(1);
    ASSERT_EQ(backend.build_topology_state(81), 0);
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    ASSERT_EQ(aclshmemx_query_topology(0, 2, &v), 0);
    const auto* borrowed = v.net_layers;
    std::vector<aclshmemx_net_layer_t> copy(borrowed, borrowed + v.net_layer_count);
    e.topology_context->rank_to_sync_endpoints.clear(); // Query must use the published snapshot.
    const int calls = gathers;
    for (int n = 0; n < 20; ++n) {
        ASSERT_EQ(aclshmemx_query_topology(0, 2, &v), 0);
        EXPECT_EQ(v.net_layers, borrowed); EXPECT_EQ(v.net_layers[0], ACLSHMEM_NET_LAYER_3);
    }
    EXPECT_EQ(gathers, calls);
    ASSERT_EQ(backend.release_aclshmem_entity(81), 0);
    EXPECT_EQ(aclshmemx_query_topology(0, 2, &v), ACLSHMEM_NOT_INITED);
    EXPECT_EQ(copy, (std::vector<aclshmemx_net_layer_t>{ACLSHMEM_NET_LAYER_3}));
    // Do not dereference borrowed after release.
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] RfcContractTest.Rfc4SnapshotIsStableUntilFixtureOwnerRelease
[PASS] RfcContractTest.Rfc4ActiveInstanceSwitchAndRepeatedFixtureCreation
[PASS] RfcContractTest.Rfc4MissingContextStillAgreesBeforeReturning
```

这几条测试的bind/release是最小fixture，不是实际设备init/finalize。它们证明查询选择与数组借用逻辑，不证明真实设备回滚和多线程生命周期安全。

### 4.5 OpenDevice两类入口的实际执行结果

[run_udma_merge_tests.py](/Users/Admin/Projects/shmem1019/tests/topology/run_udma_merge_tests.py) 提取实际OpenDevice、PrepareOpenDevice及context安装函数体，连接真实Initialize/router；设备身份、bootstrap和CreateEndpoint为桩。

已执行：借用context不再次发现；自有context成功创建并走同一builder；无路由回退；serverId过滤；shared jetty的QP槽位；错误传播；注入所有权。

原始日志：[udma.log](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/udma.log)

```text
PASS: 12 UDMA merge checks + 4 context injection checks + 6 OpenDevice checks (CPU stubs)
```

## 5. RFC 第5章：ABI、兼容性和集成

### 5.1 实现与断言

公开查询固定使用64字节v1下限，并在编译期断言对齐和字段偏移：

**代码位置：** [src/host/init/shmem_init.cpp:1370](/Users/Admin/Projects/shmem1019/src/host/init/shmem_init.cpp:1370)

```cpp
constexpr uint32_t TOPOLOGY_V1_SIZE = 64U;
static_assert(sizeof(void*) == 8 && alignof(aclshmemx_topology_t) == 8);
static_assert(sizeof(aclshmemx_topology_t) == TOPOLOGY_V1_SIZE);
static_assert(offsetof(aclshmemx_topology_t, struct_size) == 0);
static_assert(offsetof(aclshmemx_topology_t, abi_version) == 4);
static_assert(offsetof(aclshmemx_topology_t, engine_mask) == 8);
static_assert(offsetof(aclshmemx_topology_t, net_layer_count) == 12);
static_assert(offsetof(aclshmemx_topology_t, net_layers) == 16);
static_assert(offsetof(aclshmemx_topology_t, reserved) == 24);
```

**代码位置：** [tests/topology/rfc_requirements_test.cpp:50](/Users/Admin/Projects/shmem1019/tests/topology/rfc_requirements_test.cpp:50)

```cpp
TEST_F(RfcContractTest, Rfc1AbiInitializerAndEveryFieldOffset) {
    aclshmemx_topology_t v = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    EXPECT_EQ(sizeof(v), 64U); EXPECT_EQ(alignof(decltype(v)), 8U);
    EXPECT_EQ(offsetof(decltype(v), struct_size), 0U);
    EXPECT_EQ(offsetof(decltype(v), abi_version), 4U);
    EXPECT_EQ(offsetof(decltype(v), engine_mask), 8U);
    EXPECT_EQ(offsetof(decltype(v), net_layer_count), 12U);
    EXPECT_EQ(offsetof(decltype(v), net_layers), 16U);
    EXPECT_EQ(offsetof(decltype(v), reserved), 24U);
    EXPECT_EQ(v.struct_size, 64U); EXPECT_EQ(v.abi_version, 1U);
    EXPECT_EQ(v.engine_mask, 0U); EXPECT_EQ(v.net_layer_count, 0U); EXPECT_EQ(v.net_layers, nullptr);
    for (auto r : v.reserved) EXPECT_EQ(r, 0U);
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] RfcContractTest.Rfc1AbiInitializerAndEveryFieldOffset
[PASS] RfcContractTest.Rfc1AllMasksPreserveAllReservedAndExtendedTail
```

### 5.2 只做了源码检查的项目

[test_rfc_integration.py](/Users/Admin/Projects/shmem1019/tests/topology/test_rfc_integration.py) 检查公开包含链、Python导出接线、初始化调用顺序、CMake GLOB及非950排除规则、示例先验证后传输、查询不做collective。

本轮6条SOURCE_AUDIT通过。它们是源码结构证据，不是真实CANN构建、符号导出、打包或pybind链接的成功记录。

## 6. RFC 第6章：验收矩阵的具体证据

| 验证层次 | 本轮已执行 | 尚不能证明 |
| --- | --- | --- |
| C++ Host UT | A2 75、A3 75、950 80项 | 真实平台设备构建与运行 |
| 示例Host规划/分段 | 6项 | 设备kernel搬运/trace |
| Python高层包装 | 7项 | 实际native模块编译与对象复制 |
| UDMA/注入/OpenDevice | 22项检查 | 硬件endpoint/channel/QP |
| 源码接线 | 6项检查 | 编译链接与符号存在 |
| 九份拓扑 | 108个样本场景 | 本轮未重跑全部源PE矩阵 |
| biaoka/UBX | 全302场景 | 实际网络与真实D |
| 旧缓存注入 | 2场景 | 真实能力生成 |

A2和A3分别执行的是同一非950宏分支；不能因此宣称两类硬件都测过。C++检查启用ASan/UBSan。

## 7. RFC 第7章：工作量评估

这是规划估算，不是运行行为，没有合理的PASS/FAIL单元测试。不能用测试数量证明估算准确。

## 8. RFC 第8章：开发基线与来源

本轮记录了RFC SHA256、git基线、实际工作区源文件、依赖与编译器，以及每个命令的退出码和日志SHA256。

- [manifest.json](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/manifest.json)
- [receipts.json](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/receipts.json)

正式实现和测试代码都可根据路径审查；XML中的通过状态与原始日志可相互核对。

## 9. RFC 第9章：配置、D/U、self、CMO与查询矩阵

### 9.1 当前能力实现

**代码位置：** [src/host/entity/mem_entity_default.cpp:926](/Users/Admin/Projects/shmem1019/src/host/entity/mem_entity_default.cpp:926)

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
    // UDMA does not support self-send. A peer is UDMA-reachable only when route preparation
    // resolved at least one QP route; otherwise selection falls through to another reachable backend.
    uint32_t netLayer = 0;
    const bool udmaReach =
        remoteRank != options_.rankId && sdmaReach && ((options_.bmDataOpType & HYBM_DOP_TYPE_DEVICE_UDMA) != 0) &&
        transportManager_ != nullptr &&
        transportManager_->GetQpNetLayer(ACLSHMEM_DATA_OP_UDMA, remoteRank, 0, netLayer) == ACLSHMEM_SUCCESS;
    if (udmaReach) {
        supportDataOp |= HYBM_DOP_TYPE_DEVICE_UDMA;
    }

    return static_cast<hybm_data_op_type>(supportDataOp);
}
```

950：直接可达加入MTE，SDMA不发布D2D位；ROCE沿使能规则包含self；UDMA要求非self、D、使能、manager及QP0路由成功。输入配置自动加入基础MTE，不能把engine_config原样当输出。

### 9.2 UT：写死RFC期望，而不只照抄实现公式

**代码位置：** [tests/topology/engine_matrix_test.cpp:87](/Users/Admin/Projects/shmem1019/tests/topology/engine_matrix_test.cpp:87)

```cpp
TEST(TopologyEngineMatrix, Rfc9LiteralSelfAndRemoteExpectations)
{
#ifdef ACLSHMEM_SOC_950
    const uint32_t self[]   = {1,1,1,5,5,5,5,1,1,1,1,5,5,5,5};
    const uint32_t remote[] = {1,1,1,5,5,5,5,9,9,9,9,13,13,13,13};
#else
    const uint32_t self[]   = {1,3,3,5,5,7,7};
    const uint32_t remote[] = {1,3,3,5,5,7,7};
#endif
    auto to_public = [](uint32_t internal) {
        uint32_t result = 0;
        if (internal & HYBM_DOP_TYPE_MTE) result |= ACLSHMEM_DATA_OP_MTE;
        if (internal & HYBM_DOP_TYPE_DEVICE_SDMA) result |= ACLSHMEM_DATA_OP_SDMA;
        if (internal & HYBM_DOP_TYPE_DEVICE_RDMA) result |= ACLSHMEM_DATA_OP_ROCE;
        if (internal & HYBM_DOP_TYPE_DEVICE_UDMA) result |= ACLSHMEM_DATA_OP_UDMA;
        return result;
    };
    for (uint32_t config = 1; config <= sizeof(self)/sizeof(self[0]); ++config) {
        SCOPED_TRACE(config);
        aclshmemx_init_attr_t attr{};
        attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(config);
        shm::MemEntityDefault entity; entity.options_ = ConfigureEngines(&attr);
        shm::RouteFixture route{true}; entity.transportManager_ = &route;
        entity.direct = true; entity.options_.rankId = 0;
        EXPECT_EQ(to_public(entity.CanReachDataOperators(0)), self[config-1]);
        EXPECT_EQ(to_public(entity.CanReachDataOperators(1)), remote[config-1]);
        entity.direct = false;
        EXPECT_EQ(to_public(entity.CanReachDataOperators(1)), (config & 4U) ? 4U : 0U);
    }
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] TopologyEngineMatrix.Rfc9LiteralSelfAndRemoteExpectations
[PASS] TopologyEngineMatrix.Rfc9ExtensionFailuresAreNotSuccessfulInitializedQueries
[PASS] TopologyEngineMatrix.ConfigurationReachabilityRouteSelfAndQueryMasks
```

### 9.3 本轮真实日志中打印的值

以下从本轮biaoka-ubx.log直接读取，**不是示意值，也不是设备实测值**。这几条配置模式的引擎返回已经由C++断言核对；LINKS行的links_expectation_checked为false，属于观察值，物理匹配的独立UT证据见第3节。

| 场景 | 输入配置 | 源→目标 | query_mask | 预期engine_mask | 实际engine_mask | 实际status | 实际net_layers |
| --- | --- | --- | --- | --- | --- | --- | --- |

| biaoka_950_config04_D0 | 0x4 | 0→0 | 3 | 0x5 | 0x5 | 0 | [] |

| biaoka_950_config04_D0 | 0x4 | 0→1 | 3 | 0x4 | 0x4 | 0 | [0, 3] |

| biaoka_950_config0d_D1_U1 | 0xd | 0→0 | 3 | 0x5 | 0x5 | 0 | [] |

| biaoka_950_config0d_D1_U1 | 0xd | 0→1 | 3 | 0xd | 0xd | 0 | [0, 3] |

D0场景的self仍设D=True。因此config04的self返回MTE+ROCE=5，远端返回ROCE=4。config0d的self没有UDMA位，远端有路由时返回13。这些结果符合当前RFC 9.2。

原始记录摘录：

```json
[
  {
    "case": "biaoka_950_config04_D0",
    "engine_config": 4,
    "engine_expectation_checked": true,
    "engine_mask": 5,
    "engine_state_source": "production_capability_with_stub_D",
    "links_expectation_checked": false,
    "mode": "production_cpp",
    "net_layer_count": 0,
    "net_layers": [],
    "platform": "950",
    "query_mask": 3,
    "record_type": "query",
    "source_device_id": 0,
    "source_pe": 0,
    "status": 0,
    "target_device_id": 0,
    "target_pe": 0
  },
  {
    "case": "biaoka_950_config04_D0",
    "engine_config": 4,
    "engine_expectation_checked": true,
    "engine_mask": 4,
    "engine_state_source": "production_capability_with_stub_D",
    "links_expectation_checked": false,
    "mode": "production_cpp",
    "net_layer_count": 2,
    "net_layers": [
      0,
      3
    ],
    "platform": "950",
    "query_mask": 3,
    "record_type": "query",
    "source_device_id": 0,
    "source_pe": 0,
    "status": 0,
    "target_device_id": 1,
    "target_pe": 1
  },
  {
    "case": "biaoka_950_config0d_D1_U1",
    "engine_config": 13,
    "engine_expectation_checked": true,
    "engine_mask": 5,
    "engine_state_source": "production_capability_with_stub_D",
    "links_expectation_checked": false,
    "mode": "production_cpp",
    "net_layer_count": 0,
    "net_layers": [],
    "platform": "950",
    "query_mask": 3,
    "record_type": "query",
    "source_device_id": 0,
    "source_pe": 0,
    "status": 0,
    "target_device_id": 0,
    "target_pe": 0
  },
  {
    "case": "biaoka_950_config0d_D1_U1",
    "engine_config": 13,
    "engine_expectation_checked": true,
    "engine_mask": 13,
    "engine_state_source": "production_capability_with_stub_D",
    "links_expectation_checked": false,
    "mode": "production_cpp",
    "net_layer_count": 2,
    "net_layers": [
      0,
      3
    ],
    "platform": "950",
    "query_mask": 3,
    "record_type": "query",
    "source_device_id": 0,
    "source_pe": 0,
    "status": 0,
    "target_device_id": 1,
    "target_pe": 1
  }
]
```

## 10. RFC 第10章：初始化远端能力门禁

### 10.1 实现：本地检查后全体汇合

**代码位置：** [src/host/init/backends/shmem_init_backend.cpp:164](/Users/Admin/Projects/shmem1019/src/host/init/backends/shmem_init_backend.cpp:164)

```cpp
int aclshmemi_init_backend::validate_peer_reachability(uint64_t instance_id)
{
    std::lock_guard<std::mutex> lock(entity_map_mutex_);
    const auto it = entity_map_.find(instance_id);
    if (it == entity_map_.end() || it->second == nullptr || it->second->entity_host_state == nullptr) {
        return ACLSHMEM_NOT_INITED;
    }
    const auto* elem = it->second;
    const auto& host = *elem->entity_host_state;
    auto* boot = elem->entity_boot_handle;
    if (host.npes <= 0 || host.npes > ACLSHMEM_MAX_PES || host.mype < 0 || host.mype >= host.npes ||
        boot == nullptr || boot->allgather == nullptr) {
        return ACLSHMEM_INVALID_PARAM;
    }
    int local_status = ACLSHMEM_SUCCESS;
    for (int pe = 0; pe < host.npes; ++pe) {
        if (pe != host.mype && host.topo_list[pe] == 0) {
            local_status = ACLSHMEM_NOT_SUPPORTED;
            break;
        }
    }
    // A peer must not continue to later init collectives when another PE has
    // no remote engine. Fixed storage also keeps failure handling allocation-free.
    int statuses[ACLSHMEM_MAX_PES]{};
    if (boot->allgather(&local_status, statuses, sizeof(local_status), boot) != 0) {
        return ACLSHMEM_BOOTSTRAP_ERROR;
    }
    for (int pe = 0; pe < host.npes; ++pe) {
        if (statuses[pe] != ACLSHMEM_SUCCESS) return statuses[pe];
    }
    return ACLSHMEM_SUCCESS;
}
```

### 10.2 UT：失败在哪一步、self是否排除

**代码位置：** [tests/topology/rfc_requirements_test.cpp:99](/Users/Admin/Projects/shmem1019/tests/topology/rfc_requirements_test.cpp:99)

```cpp
TEST_F(RfcContractTest, Rfc10GateChecksEveryRemoteAndExcludesActualSelf) {
    state.topo_list[1] = static_cast<aclshmem_transport_t>(0);
    EXPECT_EQ(backend.validate_peer_reachability(81), 0);
    for (int target : {0, 2}) {
        const auto old = state.topo_list[target]; state.topo_list[target] = static_cast<aclshmem_transport_t>(0);
        EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_NOT_SUPPORTED);
        state.topo_list[target] = old;
    }
    EXPECT_EQ(gathers, 3);
    for (int count : {0, -1, ACLSHMEM_MAX_PES + 1}) {
        state.npes = count;
        EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_INVALID_PARAM);
    }
    state.npes = 3; state.mype = 3;
    EXPECT_EQ(backend.validate_peer_reachability(81), ACLSHMEM_INVALID_PARAM);
    EXPECT_EQ(gathers, 3);
}
```

**本轮执行记录：** [host/950.xml](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/host/950.xml)

```text
[PASS] RfcContractTest.Rfc10GateChecksEveryRemoteAndExcludesActualSelf
[PASS] TopologyQueryTest.RejectsZeroRemoteCapabilityAndAgreesRemoteFailures
```

本轮实际失败阶段输出：[nine-fixture-samples.log](/Users/Admin/Projects/shmem1019/tests/topology/results/rfc-ut-20261020/nine-fixture-samples.log)。初始化失败记录与query分开，不伪造成功查询。

```json
[
  {
    "case": "atlas_350_1_950_resource_failure",
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
    "engine_config": 5,
    "full_device_init": false,
    "platform": "950",
    "record_type": "init_stage",
    "stage_status": -9,
    "udma_qp_layers": [
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      []
    ]
  },
  {
    "case": "atlas_350_1_950_remote_unreachable",
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
    "engine_config": 5,
    "full_device_init": false,
    "platform": "950",
    "record_type": "init_stage",
    "stage_status": -100,
    "udma_qp_layers": [
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      []
    ]
  },
  {
    "case": "atlas_350_1_950_gate_bootstrap_failure",
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
    "engine_config": 5,
    "full_device_init": false,
    "platform": "950",
    "record_type": "init_stage",
    "stage_status": -6,
    "udma_qp_layers": [
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      [],
      []
    ]
  }
]
```

## 11. RFC 第11章：九份拓扑及本轮验收范围

本轮重新读取九份原始输入，按每份源PE0与末源PE的四种基础配置、资源/远端/bootstrap失败及config04_D0抽取108场景，包含64PE输入。另运行biaoka/UBX全部302例。

```text
PASS: 108 C++ JSON cases (real public query, ASan+UBSan)
PASS: 302 C++ JSON cases (real public query, ASan+UBSan)
```

此前所有源目标的3,441场景完整矩阵属于历史结果，本轮没有把它当成重新跑过：[历史报告](/Users/Admin/Projects/shmem1019/tests/topology/results/merged-1019-all/README.md)。

## 12. 全部32条需求索引

此表给出章节条款与上面的实现/UT展开之间的索引；部分、未运行和静态检查不改写成“全部通过”。

| 条款 | 要求 | 当前结论 | 对应证据 | 限制 |
| --- | --- | --- | --- | --- |

| 1.1 | 公开结构体、固定64字节ABI、initializer全部预留字段 | UT_PASS | `RfcContractTest.Rfc1AbiInitializerAndEveryFieldOffset` |  |

| 1.2 | 引擎/物理层分离、ROCE与SDMA位转换 | UT_PASS | `TopologyQueryTest.ConvertsEveryInternalBitAndPreservesCallerLayout`, `TopologyEngineMatrix.ConfigurationReachabilityRouteSelfAndQueryMasks` |  |

| 1.3-params | 参数、所有未定义位、长度、版本、缺失激活状态 | UT_PASS | `RfcContractTest.Rfc1AllInvalidBitsSizesVersionsAndMissingActiveState` |  |

| 1.3-ownership | 查询输出字段、全部reserved及扩展尾部保持、未请求项清零 | UT_PASS | `RfcContractTest.Rfc1AllMasksPreserveAllReservedAndExtendedTail` |  |

| 1.3-lifecycle | 缓存稳定、切换实例及释放前复制 | UT_PARTIAL | `RfcContractTest.Rfc4SnapshotIsStableUntilFixtureOwnerRelease`, `RfcContractTest.Rfc4ActiveInstanceSwitchAndRepeatedFixtureCreation` | 只测试实例fixture绑定/释放及指针借用，不等同真实init/finalize或C++并发释放。 |

| 1.4 | Python参数/查询位/异常/实例锁、返回对象 | UT_PARTIAL | `test_python_topology.py`, `test_public_include_export_and_python_native_copy_are_wired` | 高层包装动态执行；native绑定仅源码检查。缺少可用pybind11/CANN集成环境，未实际编译链接native模块。 |

| 2.1-2.2 | 公开查询示例所需的引擎、物理层与平台规则 | UT_PASS | `TopologyQueryTest.A2A3RejectsLinksEvenWhenEnginesIsAlsoRequested`, `TopologyEngineMatrix.ConfigurationReachabilityRouteSelfAndQueryMasks` |  |

| 2.3-plan | 双层前提、实际QP验证、错误传播及Host/kernel切分一致 | UT_PARTIAL | `RfcPlannerTest`, `RfcSlicesTest` | 执行当前固定QP0/1示例，不是RFC通用任意QP规划器；奇数余量沿1019规则给前段。实际传输/trace未运行。 |

| 3.1 | 当前实例/源PE与world目标、查询无collective | UT_PASS | `RfcContractTest.Rfc1CurrentInstanceWorldPeAndQueryPerformsNoCollectives` |  |

| 3.2 | 统一enum、raw layer0归一化、保留层不泄漏 | UT_PASS | `TopologyLayersTest.MergedRawLayerZeroUsesPhysicalEdgesAndAcceptsIp`, `TopologyLayersTest.LayerTwoAndDefaultNeverLeakIntoPublicInventory`, `TopologyQpContract` |  |

| 3.3 | 端口/fabric/plane边界、多端口/多EID归并 | UT_PARTIAL | `TopologyLayersTest.AggregatedAndSeparatePortDescriptionsYieldSameLayer`, `TopologyLayersTest.ClosRejectsMissingPeerPlaneAndAcceptsOneValidLaterPair` | 物理输入模拟验证，不证明真实左右IO DIE连通或完整Topo工具交付。 |

| 3.4 | RoCE同域、多PE、IP族/plane/网络实例负例 | UT_PARTIAL | `TopologyLayersTest.RoceFullFabricAllPairsWithRepeatedPhysicalIdsRemainDomainScoped`, `TopologyLayersTest.RoceRejectsDifferentNonemptyPlanesEmptyUnknownDomainsAndEid` | 验证描述匹配；不证明部署网络全互通或实时链路状态。 |

| 4.1 | 已有能力规则及配置到返回值映射 | UT_PASS | `TopologyEngineMatrix.Rfc9LiteralSelfAndRemoteExpectations`, `TopologyIdentityMatrix.SameServerGroupsAndCrossServerSuperPods` |  |

| 4.2-4.3 | 真实parser/context交换、MTE-only也能建立物理层快照 | UT_PASS | `DiscoveryTest.DiscoversIpOnlyTopologyWithoutCreatingUdmaResources`, `TopologyJsonStubTest.ReadsJsonAndQueriesRealPublicApi` |  |

| 4.4 | 共享/自有context入口及UDMA原路由约束 | UT_PARTIAL | `run_udma_merge_tests.py`, `UdmaRouteTest.MeshRejectsPeerFromAnotherServer`, `UdmaRouteTest.UbxClosRejectsPeerFromAnotherNetworkInstance` | 执行提取的OpenDevice/PrepareOpenDevice与真实builder/router；硬件endpoint和完整对象布局仍为桩。 |

| 4.5 | 八种层集合、所有边、双向端口、同层去重、独立于QP策略 | UT_PASS | `TopologyLayersTest.AllEightPhysicalLayerSetsWithDuplicatePlanesAndPorts`, `TopologyLayersTest.EnumeratesAllEdgesAndBothPlanesWithoutPolicy` |  |

| 4.6-4.7 | 初始化顺序、错误汇合、发布前缓存与查询/释放流程 | UT_PARTIAL | `test_rfc_integration.py`, `DiscoveryTest.EveryCollectiveFailureStopsFurtherExchanges`, `DiscoveryTest.Rfc4RuntimeMappingFailuresAreAgreed`, `DiscoveryTest.Rfc4ZeroEidCountAndOversizedPeerBlobStopBeforeBlobExchange` | 初始化调用顺序是源码检查；异常/门禁在提取函数执行。完整设备回滚未运行。 |

| 5.1 | 包含链、Python导出、源码集成及950编译隔离规则 | SOURCE_AUDIT | `RfcIntegrationAudit` |  |

| 5.2 | v1及更长同族布局兼容、原路由行为回归 | UT_PASS | `RfcContractTest.Rfc1AllMasksPreserveAllReservedAndExtendedTail`, `UdmaRouteTest` |  |

| 5.3 | 公开命名、未定义查询位拒绝、未来查询项不擅自实现 | UT_PASS | `RfcContractTest.Rfc1AllInvalidBitsSizesVersionsAndMissingActiveState` |  |

| 6.1-6.2 | 必需UT矩阵与fixture验收 | UT_PARTIAL | `host/A2.xml`, `host/A3.xml`, `host/950.xml`, `planner/planner.xml`, `fixtures` | 可本机验证部分通过；涉及真实设备、native构建的行仍待环境。 |

| 6.3 | 真实CANN构建、符号/pybind链接、双层通信及性能 | NOT_RUN |  |  |

| 7 | 工作量估算为规划信息，不是软件可执行要求 | NOT_APPLICABLE |  |  |

| 8 | 开发基线与验收来源追溯 | EVIDENCE | `manifest.json`, `summary.json` |  |

| 9.1-9.2 | D/U/self/自动补MTE/950 CMO规则/非950拒绝UDMA | UT_PASS | `TopologyEngineMatrix.Rfc9LiteralSelfAndRemoteExpectations`, `TopologyEngineMatrix.Rfc9ExtensionFailuresAreNotSuccessfulInitializedQueries`, `TopologyEngineMatrix.ConfigurationReachabilityRouteSelfAndQueryMasks` |  |

| 9.3 | 平台×query_mask与ABI/错误返回矩阵 | UT_PASS | `RfcContractTest`, `TopologyQueryTest` |  |

| 9.4 | 真实域身份/导入、IO DIE与硬件QP、XSCALE/HNS_1825、quiet | UT_PARTIAL | `TopologyIdentityMatrix`, `TopologyLayersTest`, `run_udma_merge_tests.py` | 身份字段和硬件资源是桩；不是实际内存导入及RDMA后端验收。 |

| 9.5 | 打印、配置与本机回归 | UT_PARTIAL | `fixtures`, `legacy-cpp.log`, `python-json.log` | C++ fixture日志打印已运行；完整设备示例二进制未运行。 |

| 10.1 | 远端零能力、排除self、远端失败/collective失败汇合 | UT_PASS | `RfcContractTest.Rfc10GateChecksEveryRemoteAndExcludesActualSelf`, `TopologyQueryTest.RejectsZeroRemoteCapabilityAndAgreesRemoteFailures` |  |

| 10.2 | 本机入口可复现、源码和输入指纹 | EVIDENCE | `run_rfc_ut.py`, `manifest.json`, `receipts.json` |  |

| 10.3 | 完整设备初始化回滚、硬件CMO/quiet/真实建链 | NOT_RUN |  |  |

| 11 | 九份原始TopoInfo、模拟RootInfo与完整PE矩阵 | UT_PARTIAL | `fixtures`, `../merged-1019-all/README.md` | 本轮九份输入执行所有源PE0/末源PE的四种基础配置与失败样本，并执行biaoka/UBX全302例；此前完整PE矩阵为历史结果，未冒充本轮重跑。 |

## 13. 可以给出的结论

当前正式实现有可定位的代码，核心接口、能力矩阵、物理匹配、共享context和门禁都有可执行UT/桩集成证据，上文给出实际测试代码和本轮打印结果。

仍未完成真实CANN/pybind构建链接、完整设备init/finalize回滚、硬件QP/channel、RDMA两后端、CMO/quiet数据面、真实IO DIE/RoCE全互通、双层实际流量和性能验收。因此结论是“本机可测逻辑通过、部分需求仍待外部验收”，而不是“整个RFC已经全部满足”。
