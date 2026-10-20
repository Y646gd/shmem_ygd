/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

// Test-only access override scoped strictly to the two under-api headers, so this fixture can
// replace their dynamically loaded entry points with local recorders. Pre-include the transitive
// standard headers first: dl_acl_api.h pulls in the logger's <sstream> via shmemi_functions.h,
// and a class redeclared with different access under the override is a hard error. Keep the undef
// right after these includes and place no other header inside this scope.
#include <mutex>
#include <sstream>
#define private public
#include "dl_hcomm_api.h"
#include "dl_acl_api.h"
#undef private

#include "device_udma_transport_manager.h"
#include "shmemi_host_common.h"

namespace shm {
namespace transport {
namespace device {

class UdmaEndpointExchangeTest : public testing::Test {
protected:
    using Descriptor = UdmaTransportManager::ExchangedEndpointDesc;
    using Exchange = UdmaTransportManager::EndpointExchange;
    using JettyStateMaps = UdmaTransportManager::JettyStateMaps;
    void SetUp() override
    {
        saved_handle_ = g_boot_handle;
        saved_mem_reg_ = DlHcommApi::gHcommMemReg;
        saved_mem_unreg_ = DlHcommApi::gHcommMemUnreg;
        saved_endpoint_destroy_ = DlHcommApi::gHcommEndpointDestroy;
        saved_channel_create_ = DlHcommApi::gHcommChannelCreate;
        saved_channel_destroy_ = DlHcommApi::gHcommChannelDestroy;
        saved_config_create_ = DlHcommApi::gHcommChannelConfigCreate;
        saved_config_destroy_ = DlHcommApi::gHcommChannelConfigDestroy;
        saved_config_set_int_ = DlHcommApi::gHcommChannelConfigSetInt;
        saved_channel_create_with_config_ = DlHcommApi::gHcommChannelCreateWithConfig;
        saved_acl_malloc_ = DlAclApi::pAclrtMalloc;
        saved_acl_memset_ = DlAclApi::pAclrtMemset;
        saved_acl_free_ = DlAclApi::pAclrtFree;
        g_boot_handle.npes = 3;
        g_boot_handle.allgather = AllGather;
        lengths_.clear();
        fail_call_ = 0;
        mem_reg_endpoints_.clear();
        channel_endpoints_.clear();
        config_create_calls_ = 0;
        config_set_int_calls_.clear();
        channel_create_with_config_endpoints_.clear();
        legacy_channel_create_calls_ = 0;
        next_handle_ = 1000;
        DlHcommApi::gHcommMemReg = RecordMemReg;
        DlHcommApi::gHcommMemUnreg = SucceedMemUnreg;
        DlHcommApi::gHcommEndpointDestroy = SucceedEndpointDestroy;
        DlHcommApi::gHcommChannelCreate = RecordChannelCreate;
        DlHcommApi::gHcommChannelDestroy = SucceedChannelDestroy;
        // Soft channel-config symbols start null: a test opts in by installing the recorders,
        // so the default state mirrors an old libhcomm.so without the shared-jetty APIs.
        DlHcommApi::gHcommChannelConfigCreate = nullptr;
        DlHcommApi::gHcommChannelConfigDestroy = nullptr;
        DlHcommApi::gHcommChannelConfigSetInt = nullptr;
        DlHcommApi::gHcommChannelCreateWithConfig = nullptr;
        DlAclApi::pAclrtMalloc = nullptr;
        DlAclApi::pAclrtMemset = nullptr;
        DlAclApi::pAclrtFree = nullptr;
    }

    void TearDown() override
    {
        DlHcommApi::gHcommMemReg = saved_mem_reg_;
        DlHcommApi::gHcommMemUnreg = saved_mem_unreg_;
        DlHcommApi::gHcommEndpointDestroy = saved_endpoint_destroy_;
        DlHcommApi::gHcommChannelCreate = saved_channel_create_;
        DlHcommApi::gHcommChannelDestroy = saved_channel_destroy_;
        DlHcommApi::gHcommChannelConfigCreate = saved_config_create_;
        DlHcommApi::gHcommChannelConfigDestroy = saved_config_destroy_;
        DlHcommApi::gHcommChannelConfigSetInt = saved_config_set_int_;
        DlHcommApi::gHcommChannelCreateWithConfig = saved_channel_create_with_config_;
        DlAclApi::pAclrtMalloc = saved_acl_malloc_;
        DlAclApi::pAclrtMemset = saved_acl_memset_;
        DlAclApi::pAclrtFree = saved_acl_free_;
        g_boot_handle = saved_handle_;
    }

    static int AllGather(const void* send, void* recv, int len, aclshmemi_bootstrap_handle_t* handle)
    {
        lengths_.push_back(len);
        if (lengths_.size() == fail_call_) {
            return ACLSHMEM_BOOTSTRAP_ERROR;
        }
        const size_t count = static_cast<size_t>(len) / sizeof(Descriptor);
        const auto* input = static_cast<const Descriptor*>(send);
        auto* output = static_cast<Descriptor*>(recv);
        for (int rank = 0; rank < handle->npes; ++rank) {
            for (size_t idx = 0; idx < count; ++idx) {
                output[rank * count + idx] = input[idx];
            }
        }
        return ACLSHMEM_SUCCESS;
    }

    static int MirrorAllGather(const void* send, void* recv, int len, aclshmemi_bootstrap_handle_t* handle)
    {
        for (int rank = 0; rank < handle->npes; ++rank) {
            std::memcpy(static_cast<uint8_t*>(recv) + static_cast<size_t>(rank) * len, send, len);
        }
        return ACLSHMEM_SUCCESS;
    }

    static Result Gather(uint32_t chunk_count, Exchange& exchange)
    {
        exchange.max_count = 5;
        std::vector<Descriptor> local(exchange.max_count);
        for (uint32_t idx = 0; idx < exchange.max_count; ++idx) {
            local[idx].eid_index = idx;
            local[idx].valid = idx < 4 ? 1 : 0;
            local[idx].listen_port = static_cast<uint16_t>(100 + idx);
        }
        return UdmaTransportManager::GatherEndpointChunks(local, 3, chunk_count, exchange);
    }

    static uint32_t ChunkCount(uint32_t ranks) { return UdmaTransportManager::GetEndpointChunkCount(ranks); }

    static bool GetSize(uint32_t ranks, uint32_t endpoints, size_t& count)
    {
        return UdmaTransportManager::GetEndpointExchangeSize(ranks, endpoints, count);
    }

    static uint32_t MaxEndpoints()
    {
        return std::numeric_limits<int>::max() / sizeof(UdmaTransportManager::ExchangedEndpointDesc);
    }

    static EndpointHandle Handle(uintptr_t value) { return reinterpret_cast<EndpointHandle>(value); }

    static HcommResult RecordMemReg(EndpointHandle endpoint, const char*, CommMem*, HcommMemHandle* mem_handle)
    {
        mem_reg_endpoints_.push_back(endpoint);
        *mem_handle = reinterpret_cast<HcommMemHandle>(next_handle_++);
        return 0;
    }

    static HcommResult SucceedMemUnreg(EndpointHandle, HcommMemHandle) { return 0; }
    static HcommResult SucceedEndpointDestroy(EndpointHandle) { return 0; }
    static HcommResult SucceedChannelDestroy(const ChannelHandle*, uint32_t) { return 0; }

    // Shared fake handle recording for the two channel-create recorders below.
    static void RecordChannelHandles(EndpointHandle endpoint, uint32_t count, ChannelHandle* channels)
    {
        for (uint32_t idx = 0; idx < count; ++idx) {
            channel_endpoints_.push_back(endpoint);
            channels[idx] = next_handle_++;
        }
    }

    static HcommResult RecordChannelCreate(
        EndpointHandle endpoint, CommEngine, HcommChannelDesc*, uint32_t count, ChannelHandle* channels)
    {
        ++legacy_channel_create_calls_;
        RecordChannelHandles(endpoint, count, channels);
        return 0;
    }

    // Channel-config API recorders, installed by tests that exercise the shared-jetty path.
    // The config handle is opaque, so a distinct fake pointer per create is enough.
    static HcommResult RecordConfigCreate(HcommChannelConfig* config)
    {
        ++config_create_calls_;
        *config = reinterpret_cast<HcommChannelConfig>(next_handle_++);
        return 0;
    }

    static HcommResult RecordConfigDestroy(HcommChannelConfig) { return 0; }

    static HcommResult RecordConfigSetInt(HcommChannelConfig, HcommChannelConfigType type, uint32_t value)
    {
        config_set_int_calls_.push_back({static_cast<uint32_t>(type), value});
        return 0;
    }

    static HcommResult RecordChannelCreateWithConfig(
        EndpointHandle endpoint, CommEngine, HcommChannelDesc*, uint32_t count, HcommChannelConfig,
        ChannelHandle* channels)
    {
        channel_create_with_config_endpoints_.push_back(endpoint);
        RecordChannelHandles(endpoint, count, channels);
        return 0;
    }

    void InstallChannelConfigApis()
    {
        DlHcommApi::gHcommChannelConfigCreate = RecordConfigCreate;
        DlHcommApi::gHcommChannelConfigDestroy = RecordConfigDestroy;
        DlHcommApi::gHcommChannelConfigSetInt = RecordConfigSetInt;
        DlHcommApi::gHcommChannelCreateWithConfig = RecordChannelCreateWithConfig;
    }

    // ACL stubs backing AllocateAndClearDeviceBuffer: one real host buffer per queue state, so
    // the zeroing memset actually lands and the freed pointers stay valid for later checks.
    static int32_t RecordAclMalloc(void** ptr, size_t size, uint32_t)
    {
        *ptr = new (std::nothrow) uint8_t[size]{};
        return *ptr == nullptr ? 1 : 0;
    }

    static int32_t RecordAclMemset(void* dst, size_t destMax, int32_t value, size_t count)
    {
        std::memset(dst, static_cast<int>(value), count);
        (void)destMax;
        return 0;
    }

    static int RecordAclFree(void* ptr)
    {
        delete[] static_cast<uint8_t*>(ptr);
        return 0;
    }

    void InstallAclAllocApis()
    {
        DlAclApi::pAclrtMalloc = RecordAclMalloc;
        DlAclApi::pAclrtMemset = RecordAclMemset;
        DlAclApi::pAclrtFree = RecordAclFree;
    }

    static SqContext MakeSqContext(uint32_t jfs_id, uint64_t sq_va, uint64_t db_va)
    {
        SqContext context{};
        context.type = SQ_CONTEXT_TYPE_UB_JFS;
        context.contextInfo.ubJfs.jfsID = jfs_id;
        context.contextInfo.ubJfs.sqVa = sq_va;
        context.contextInfo.ubJfs.dbVa = db_va;
        return context;
    }

    static Result ResolveQueueStateBindingForTest(
        UdmaTransportManager& manager, const SqContext& sq_context, uint32_t dst_pe, uint32_t qp_idx, uint64_t slot,
        size_t& state_idx, UdmaTransportManager::JettyStateMaps& jetty_maps)
    {
        return manager.ResolveQueueStateBinding(sq_context, dst_pe, qp_idx, slot, state_idx, jetty_maps);
    }

    static Result BindQueueStateAddrForTest(
        UdmaTransportManager& manager, size_t state_idx, uint64_t slot, uint32_t qp_idx, aclshmemi_udma_wq_ctx_t& wq,
        aclshmemi_udma_cq_ctx_t& cq)
    {
        return manager.BindQueueStateAddr(state_idx, slot, qp_idx, wq, cq);
    }

    static void ConfigureSharedJetty(
        UdmaTransportManager& manager, uint32_t rank, uint32_t ranks, uint32_t qp_num, bool shared_jetty)
    {
        manager.rank_id_ = rank;
        manager.rank_count_ = ranks;
        manager.qp_num_ = qp_num;
        manager.shared_jetty_enabled_ = shared_jetty;
    }

    static size_t QueueStateCount(const UdmaTransportManager& manager) { return manager.queue_state_dev_list_.size(); }

    // Append a raw entry to the manager's state list; only used to model a never-allocated slot.
    static size_t AppendNullQueueState(UdmaTransportManager& manager)
    {
        manager.queue_state_dev_list_.push_back(nullptr);
        return manager.queue_state_dev_list_.size() - 1;
    }

    static void AddEndpoint(UdmaTransportManager& manager, uint32_t eid, uint32_t /*peer*/, uintptr_t handle)
    {
        AddEndpointWithSlot(manager, eid, 0U, handle);
    }

    static void AddEndpointWithSlot(UdmaTransportManager& manager, uint32_t eid, uint32_t qp_slot, uintptr_t handle)
    {
        const auto key = UdmaTransportManager::EndpointKey{eid, qp_slot};
        EndpointDesc desc{};
        manager.endpoint_handle_map_[key] = Handle(handle);
        manager.endpoint_desc_map_[key] = desc;
        manager.endpoint_listen_port_map_[key] = static_cast<uint16_t>(1000 + handle);
    }

    static size_t RegisteredEndpointCount(const UdmaTransportManager& manager, uint64_t addr)
    {
        const auto region = manager.mem_record_map_.find(addr);
        return region == manager.mem_record_map_.end() ? 0U : region->second.size();
    }

    static size_t LogicalEndpointCount(const UdmaTransportManager& manager)
    {
        return manager.endpoint_desc_map_.size();
    }

    static void ConfigureManager(UdmaTransportManager& manager, uint32_t rank, uint32_t ranks, uint32_t qp_num)
    {
        manager.rank_id_ = rank;
        manager.rank_count_ = ranks;
        manager.qp_num_ = qp_num;
    }

    static void AddRoute(
        UdmaTransportManager& manager, uint32_t peer, uint32_t local_eid, uint32_t remote_eid, uint32_t net_layer)
    {
        manager.peer_routes_[peer].qp_routes.push_back(UdmaTransportManager::QpRoute{
            local_eid, remote_eid, NetType::TopoFileDesc, net_layer, "instance", "plane", 1});
    }

    static void AddRemoteDescriptor(Exchange& exchange, uint32_t rank, uint32_t index, uint32_t eid)
    {
        AddRemoteDescriptorWithSlot(exchange, rank, index, eid, 0U);
    }

    static void AddRemoteDescriptorWithSlot(
        Exchange& exchange, uint32_t rank, uint32_t index, uint32_t eid, uint32_t qp_slot)
    {
        Descriptor& descriptor = exchange.descs[static_cast<size_t>(rank) * exchange.max_count + index];
        descriptor.valid = 1;
        descriptor.eid_index = eid;
        descriptor.qp_slot = qp_slot;
        descriptor.listen_port = static_cast<uint16_t>(2000 + eid);
    }

    static Result BuildDirect(
        UdmaTransportManager& manager, const Exchange& exchange, std::vector<uint32_t>& slots,
        std::vector<uint32_t>& dst_pes, std::vector<uint32_t>& qp_indices)
    {
        UdmaTransportManager::ChannelBuildState state;
        const Result result = manager.BuildDirectChannels(exchange, state);
        slots = state.slots;
        dst_pes = state.dst_pes;
        qp_indices = state.qp_indices;
        return result;
    }

    aclshmemi_bootstrap_handle_t saved_handle_{};
    HcommMemRegFunc saved_mem_reg_{nullptr};
    HcommMemUnregFunc saved_mem_unreg_{nullptr};
    HcommEndpointDestroyFunc saved_endpoint_destroy_{nullptr};
    HcommChannelCreateFunc saved_channel_create_{nullptr};
    HcommChannelDestroyFunc saved_channel_destroy_{nullptr};
    HcommChannelConfigCreateFunc saved_config_create_{nullptr};
    HcommChannelConfigDestroyFunc saved_config_destroy_{nullptr};
    HcommChannelConfigSetIntFunc saved_config_set_int_{nullptr};
    HcommChannelCreateWithConfigFunc saved_channel_create_with_config_{nullptr};
    aclrtMallocFunc saved_acl_malloc_{nullptr};
    aclrtMemsetFunc saved_acl_memset_{nullptr};
    aclrtFreeFunc saved_acl_free_{nullptr};
    inline static std::vector<int> lengths_;
    inline static size_t fail_call_{0};
    inline static std::vector<EndpointHandle> mem_reg_endpoints_;
    inline static std::vector<EndpointHandle> channel_endpoints_;
    inline static size_t config_create_calls_{0};
    inline static std::vector<std::pair<uint32_t, uint32_t>> config_set_int_calls_;
    inline static std::vector<EndpointHandle> channel_create_with_config_endpoints_;
    inline static size_t legacy_channel_create_calls_{0};
    inline static uintptr_t next_handle_{1000};
};

TEST_F(UdmaEndpointExchangeTest, AcceptsBoundaryAndRejectsNextEndpoint)
{
    constexpr uint32_t ranks = 1;
    const uint32_t endpoints = MaxEndpoints();
    size_t count = 0;
    ASSERT_TRUE(GetSize(ranks, endpoints, count));
    EXPECT_EQ(count, static_cast<size_t>(ranks) * endpoints);
    EXPECT_FALSE(GetSize(ranks, endpoints + 1, count));
    EXPECT_EQ(count, 0U);
}

TEST_F(UdmaEndpointExchangeTest, KeepsExplicitClosFabricWhenPortsAlsoMatchMeshTopology)
{
    // PoD may expose an aggregated CLOS EID whose ports also occur in the
    // Mesh topology. The rootinfo CLOS declaration must take precedence;
    // only TOPO_FILE_DESC endpoints infer their fabric from topology ports.
    g_boot_handle.npes = 1;
    g_boot_handle.allgather = MirrorAllGather;

    RootInfo root_info;
    auto level = std::make_shared<LevelInfo>(LevelInfo{1, "clos-domain", NetType::Clos, ""});
    auto rank_addr = std::make_shared<RankAddr>();
    rank_addr->levelInfo = level;
    rank_addr->ports = {"shared-port"};
    root_info.eidIndexToRankAddr.emplace(7, rank_addr);

    TopoInfo topo_info;
    topo_info.meshTopoEdges.push_back({0, 1, {"shared-port"}, {"peer-mesh-port"}});
    topo_info.closTopoEdges.push_back({1, 0, 0, {"shared-port"}});

    const auto endpoints = TopologyContext::BuildLocalEndpoints(root_info, topo_info);
    ASSERT_EQ(endpoints.size(), 1U);
    EXPECT_EQ(endpoints[0].netType, NetType::Clos);
    EXPECT_EQ(endpoints[0].fabric, TopoFabric::Clos);
}

TEST_F(UdmaEndpointExchangeTest, RejectsEmptyAndOverflowingDimensions)
{
    size_t count = 1;
    EXPECT_FALSE(GetSize(0, 1, count));
    EXPECT_EQ(count, 0U);
    EXPECT_FALSE(GetSize(1, 0, count));
    EXPECT_FALSE(GetSize(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), count));
    EXPECT_EQ(count, 0U);
}

TEST_F(UdmaEndpointExchangeTest, AcceptsDescriptorMatricesAboveEightMiB)
{
    size_t count = 0;
    EXPECT_TRUE(GetSize(8, 7, count));
    EXPECT_EQ(count, 56U);
    EXPECT_TRUE(GetSize(256, 255, count));
    EXPECT_EQ(count, 256U * 255U);
    EXPECT_TRUE(GetSize(1024, 1023, count));
    EXPECT_EQ(count, 1024U * 1023U);
    EXPECT_TRUE(GetSize(1024, 8U * 1023U, count));
    EXPECT_EQ(count, 1024U * 8U * 1023U);
}

TEST_F(UdmaEndpointExchangeTest, SplitsLargeClusterMatricesWithoutAllocatingThem)
{
    for (uint32_t ranks : {3493U, 3494U, 4096U, 10000U, 16384U}) {
        size_t count = 0;
        ASSERT_TRUE(GetSize(ranks, ranks - 1, count));
        const uint32_t chunk = ChunkCount(ranks);
        ASSERT_GT(chunk, 0U);
        EXPECT_LT(chunk, ranks - 1);
        // Mirror config_store's int multiplication under UBSan.
        const int bytes = static_cast<int>(chunk * sizeof(Descriptor));
        const int receive_bytes = bytes * static_cast<int>(ranks);
        EXPECT_LE(receive_bytes, 8 * 1024 * 1024);
    }
    EXPECT_EQ(ChunkCount(0), 0U);
}

TEST_F(UdmaEndpointExchangeTest, PreservesRankStridePaddingAndTailAcrossChunks)
{
    Exchange exchange;
    ASSERT_EQ(Gather(2, exchange), ACLSHMEM_SUCCESS);
    EXPECT_EQ(lengths_, (std::vector<int>{2 * sizeof(Descriptor), 2 * sizeof(Descriptor), sizeof(Descriptor)}));
    ASSERT_EQ(exchange.descs.size(), 15U);
    for (uint32_t rank = 0; rank < 3; ++rank) {
        for (uint32_t idx = 0; idx < 5; ++idx) {
            const auto& desc = exchange.descs[rank * 5 + idx];
            EXPECT_EQ(desc.eid_index, idx);
            EXPECT_EQ(desc.valid, idx < 4 ? 1U : 0U);
            EXPECT_EQ(desc.listen_port, 100U + idx);
        }
    }
}

TEST_F(UdmaEndpointExchangeTest, SingleChunkMatchesSplitExchange)
{
    Exchange split;
    ASSERT_EQ(Gather(2, split), ACLSHMEM_SUCCESS);
    lengths_.clear();
    Exchange single;
    ASSERT_EQ(Gather(5, single), ACLSHMEM_SUCCESS);
    EXPECT_EQ(lengths_.size(), 1U);
    ASSERT_EQ(single.descs.size(), split.descs.size());
    for (size_t idx = 0; idx < split.descs.size(); ++idx) {
        EXPECT_EQ(single.descs[idx].eid_index, split.descs[idx].eid_index);
        EXPECT_EQ(single.descs[idx].valid, split.descs[idx].valid);
    }
}

TEST_F(UdmaEndpointExchangeTest, StopsAtFailedChunk)
{
    fail_call_ = 2;
    Exchange exchange;
    EXPECT_EQ(Gather(2, exchange), ACLSHMEM_BOOTSTRAP_ERROR);
    EXPECT_EQ(lengths_.size(), 2U);
}

TEST_F(UdmaEndpointExchangeTest, RegistersOneHeapOnEverySelectedUniqueEndpoint)
{
    UdmaTransportManager manager;
    AddEndpoint(manager, 10, 1, 1);
    AddEndpoint(manager, 11, 1, 2);
    AddEndpoint(manager, 12, 2, 3);

    TransportMemoryRegion region{};
    region.addr = 0x100000;
    region.size = 0x2000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);

    EXPECT_EQ(mem_reg_endpoints_, (std::vector<EndpointHandle>{Handle(1), Handle(2), Handle(3)}));
    EXPECT_EQ(RegisteredEndpointCount(manager, region.addr), 3U);
}

TEST_F(UdmaEndpointExchangeTest, RegistersSharedLocalEidOnlyOnceAcrossPeers)
{
    UdmaTransportManager manager;
    // Same (eid, qp_slot) key: the second/third AddEndpoint calls overwrite the same map slot.
    // This models the same EID reused by routes to multiple peers (eid-only sharing on the
    // upstream keying collapses to the identical (eid, 0) entry here).
    AddEndpoint(manager, 7, 1, 7);
    AddEndpoint(manager, 7, 2, 7);
    AddEndpoint(manager, 7, 3, 7);

    TransportMemoryRegion region{};
    region.addr = 0x100000;
    region.size = 0x2000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);

    EXPECT_EQ(mem_reg_endpoints_, (std::vector<EndpointHandle>{Handle(7)}));
    EXPECT_EQ(RegisteredEndpointCount(manager, region.addr), 1U);
    EXPECT_EQ(LogicalEndpointCount(manager), 1U);
}

TEST_F(UdmaEndpointExchangeTest, BuildsOneChannelForEveryPeerQpFromFinalRoutes)
{
    UdmaTransportManager manager;
    ConfigureManager(manager, 0, 3, 3);

    // Repeated local EIDs model two QPs sharing one endpoint. The channel count
    // must still be peer_count * qp_num, while endpoint/MR resources are reused.
    AddRoute(manager, 1, 10, 20, 0);
    AddRoute(manager, 1, 10, 20, 0);
    AddRoute(manager, 1, 11, 21, 1);
    AddRoute(manager, 2, 12, 22, 0);
    AddRoute(manager, 2, 13, 23, 1);
    AddRoute(manager, 2, 12, 22, 0);
    AddEndpoint(manager, 10, 1, 10);
    AddEndpoint(manager, 11, 1, 11);
    AddEndpoint(manager, 12, 2, 12);
    AddEndpoint(manager, 13, 2, 13);

    TransportMemoryRegion region{};
    region.addr = 0x200000;
    region.size = 0x4000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);
    ASSERT_EQ(mem_reg_endpoints_.size(), 4U);

    Exchange exchange;
    exchange.max_count = 3;
    exchange.counts = {0, 3, 3};
    exchange.descs.resize(9);
    AddRemoteDescriptor(exchange, 1, 0, 20);
    AddRemoteDescriptor(exchange, 1, 1, 21);
    AddRemoteDescriptor(exchange, 2, 0, 22);
    AddRemoteDescriptor(exchange, 2, 1, 23);

    std::vector<uint32_t> slots;
    std::vector<uint32_t> dst_pes;
    std::vector<uint32_t> qp_indices;
    ASSERT_EQ(BuildDirect(manager, exchange, slots, dst_pes, qp_indices), ACLSHMEM_SUCCESS);

    EXPECT_EQ(
        channel_endpoints_,
        (std::vector<EndpointHandle>{Handle(10), Handle(10), Handle(11), Handle(12), Handle(13), Handle(12)}));
    EXPECT_EQ(slots, (std::vector<uint32_t>{1, 1, 1, 2, 2, 2}));
    EXPECT_EQ(dst_pes, (std::vector<uint32_t>{1, 1, 1, 2, 2, 2}));
    EXPECT_EQ(qp_indices, (std::vector<uint32_t>{0, 1, 2, 0, 1, 2}));
}

// Direct-build only: shared jetty is rejected at OpenDevice in relay builds, and BuildChannels
// dispatches to the relay variant there, so this scenario cannot exist.
#if !defined(ACLSHMEM_RELAY_SUPPORT)
TEST_F(UdmaEndpointExchangeTest, BuildsChannelsThroughConfigApisWhenSharedJettyEnabled)
{
    UdmaTransportManager manager;
    ConfigureSharedJetty(manager, 0, 3, 3, true);
    InstallChannelConfigApis();

    // Shared jetty + qp_num > 1 splits one endpoint per (eid, qp_slot), so qp1 reuses eid 10
    // through a distinct key while qp0/qp2 egress on other EIDs.
    AddEndpointWithSlot(manager, 10, 0, 10);
    AddEndpointWithSlot(manager, 10, 1, 11);
    AddEndpointWithSlot(manager, 11, 2, 12);
    AddRoute(manager, 1, 10, 20, 0);
    AddRoute(manager, 1, 10, 20, 1);
    AddRoute(manager, 1, 11, 21, 2);
    AddRoute(manager, 2, 12, 22, 0);
    AddRoute(manager, 2, 13, 23, 1);
    AddRoute(manager, 2, 12, 22, 2);
    AddEndpointWithSlot(manager, 12, 0, 12);
    AddEndpointWithSlot(manager, 13, 1, 13);
    AddEndpointWithSlot(manager, 12, 2, 14);

    TransportMemoryRegion region{};
    region.addr = 0x200000;
    region.size = 0x4000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);

    Exchange exchange;
    exchange.max_count = 3;
    exchange.counts = {0, 3, 3};
    exchange.descs.resize(9);
    AddRemoteDescriptorWithSlot(exchange, 1, 0, 20, 0);
    AddRemoteDescriptorWithSlot(exchange, 1, 1, 20, 1);
    AddRemoteDescriptorWithSlot(exchange, 1, 2, 21, 2);
    AddRemoteDescriptorWithSlot(exchange, 2, 0, 22, 0);
    AddRemoteDescriptorWithSlot(exchange, 2, 1, 23, 1);
    AddRemoteDescriptorWithSlot(exchange, 2, 2, 22, 2);

    std::vector<uint32_t> slots;
    std::vector<uint32_t> dst_pes;
    std::vector<uint32_t> qp_indices;
    ASSERT_EQ(BuildDirect(manager, exchange, slots, dst_pes, qp_indices), ACLSHMEM_SUCCESS);

    // Every batch went through the config path with IS_SHARED_QUEUE = 1; none fell back to the
    // legacy create (which would silently drop the sharing request).
    EXPECT_GT(config_create_calls_, 0U);
    EXPECT_EQ(
        channel_create_with_config_endpoints_,
        (std::vector<EndpointHandle>{Handle(10), Handle(11), Handle(12), Handle(12), Handle(13), Handle(14)}));
    EXPECT_EQ(legacy_channel_create_calls_, 0U);
    for (const auto& call : config_set_int_calls_) {
        EXPECT_EQ(call.first, static_cast<uint32_t>(HCOMM_CHANNEL_CONFIG_TYPE_IS_SHARED_QUEUE));
        EXPECT_EQ(call.second, 1U);
    }
    EXPECT_EQ(slots, (std::vector<uint32_t>{1, 1, 1, 2, 2, 2}));
    EXPECT_EQ(qp_indices, (std::vector<uint32_t>{0, 1, 2, 0, 1, 2}));
}
#endif

TEST_F(UdmaEndpointExchangeTest, KeepsLegacyChannelCreateWhenSharedJettyDisabled)
{
    UdmaTransportManager manager;
    ConfigureSharedJetty(manager, 0, 3, 1, false);
    InstallChannelConfigApis();

    AddRoute(manager, 1, 10, 20, 0);
    AddRoute(manager, 2, 11, 21, 0);
    AddEndpoint(manager, 10, 1, 10);
    AddEndpoint(manager, 11, 2, 11);

    TransportMemoryRegion region{};
    region.addr = 0x200000;
    region.size = 0x4000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);

    Exchange exchange;
    exchange.max_count = 2;
    exchange.counts = {0, 1, 1};
    exchange.descs.resize(6);
    AddRemoteDescriptor(exchange, 1, 0, 20);
    AddRemoteDescriptor(exchange, 2, 0, 21);

    std::vector<uint32_t> slots;
    std::vector<uint32_t> dst_pes;
    std::vector<uint32_t> qp_indices;
    ASSERT_EQ(BuildDirect(manager, exchange, slots, dst_pes, qp_indices), ACLSHMEM_SUCCESS);

    EXPECT_EQ(legacy_channel_create_calls_, 2U);
    EXPECT_EQ(config_create_calls_, 0U);
    EXPECT_EQ(channel_create_with_config_endpoints_.size(), 0U);
}

TEST_F(UdmaEndpointExchangeTest, SkipsPeersWithoutUdmaRoutes)
{
    UdmaTransportManager manager;
    ConfigureManager(manager, 0, 3, 2);

    AddRoute(manager, 1, 10, 20, 0);
    AddRoute(manager, 1, 11, 21, 1);
    // 本地 AddEndpoint 带 qp_slot 兼容参数（peer 位未用），等价于 upstream 的三参形式。
    AddEndpoint(manager, 10, 0, 10);
    AddEndpoint(manager, 11, 0, 11);

    TransportMemoryRegion region{};
    region.addr = 0x200000;
    region.size = 0x4000;
    region.flags = REG_MR_FLAG_HBM;
    ASSERT_EQ(manager.RegisterMemoryRegion(region), ACLSHMEM_SUCCESS);
    ASSERT_EQ(mem_reg_endpoints_.size(), 2U);

    Exchange exchange;
    exchange.max_count = 2;
    exchange.counts = {0, 2, 0};
    exchange.descs.resize(6);
    AddRemoteDescriptor(exchange, 1, 0, 20);
    AddRemoteDescriptor(exchange, 1, 1, 21);

    std::vector<uint32_t> slots;
    std::vector<uint32_t> dst_pes;
    std::vector<uint32_t> qp_indices;
    ASSERT_EQ(BuildDirect(manager, exchange, slots, dst_pes, qp_indices), ACLSHMEM_SUCCESS);

    EXPECT_EQ(channel_endpoints_, (std::vector<EndpointHandle>{Handle(10), Handle(11)}));
    EXPECT_EQ(slots, (std::vector<uint32_t>{1, 1}));
    EXPECT_EQ(dst_pes, (std::vector<uint32_t>{1, 1}));
    EXPECT_EQ(qp_indices, (std::vector<uint32_t>{0, 1}));
}

// The split/duplicate defenses only exist on the direct multi-QP path (the checks are compiled
// out of relay builds); the jetty-key dedup itself is relay-agnostic and covered by the first
// two sections below, so the whole test stays direct-only for a single consistent expectation.
#if !defined(ACLSHMEM_RELAY_SUPPORT)
TEST_F(UdmaEndpointExchangeTest, ResolvesOneQueueStatePerDistinctJetty)
{
    UdmaTransportManager manager;
    ConfigureSharedJetty(manager, 0, 3, 2, true);
    InstallAclAllocApis();

    JettyStateMaps jetty_maps;
    size_t state_a = SIZE_MAX;
    size_t state_b = SIZE_MAX;
    // Same jetty key from two slots of one QP (pe 1 and pe 2 both resolve to jetty (5, 0x1000,
    // 0x2000)): legal sharing, both must land on the same state buffer.
    const SqContext shared_jetty = MakeSqContext(5, 0x1000, 0x2000);
    ASSERT_EQ(ResolveQueueStateBindingForTest(manager, shared_jetty, 1, 0, 1, state_a, jetty_maps), ACLSHMEM_SUCCESS);
    ASSERT_EQ(ResolveQueueStateBindingForTest(manager, shared_jetty, 2, 0, 2, state_b, jetty_maps), ACLSHMEM_SUCCESS);
    EXPECT_EQ(state_a, state_b);
    EXPECT_EQ(QueueStateCount(manager), 1U);

    // A distinct jetty key allocates a second state buffer and gets a different index.
    size_t state_c = SIZE_MAX;
    const SqContext other_jetty = MakeSqContext(6, 0x1800, 0x2800);
    ASSERT_EQ(ResolveQueueStateBindingForTest(manager, other_jetty, 1, 1, 1, state_c, jetty_maps), ACLSHMEM_SUCCESS);
    EXPECT_NE(state_c, state_a);
    EXPECT_EQ(QueueStateCount(manager), 2U);

    // Duplicate (dst_pe, qp_idx) is a fill-twice defect, not sharing: the binding fails and the
    // already-allocated state buffers are released.
    size_t state_dup = SIZE_MAX;
    EXPECT_EQ(
        ResolveQueueStateBindingForTest(manager, shared_jetty, 1, 0, 1, state_dup, jetty_maps), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(QueueStateCount(manager), 0U);
}
#endif

#if !defined(ACLSHMEM_RELAY_SUPPORT)
TEST_F(UdmaEndpointExchangeTest, RejectsJettyMergedAcrossQps)
{
    // The per-QP endpoint split relies on hcomm keeping same-peer QPs on separate jetties. If it
    // merges them anyway, two different QPs would race on one state buffer: the bind must fail.
    UdmaTransportManager manager;
    ConfigureSharedJetty(manager, 0, 3, 2, true);
    InstallAclAllocApis();

    JettyStateMaps jetty_maps;
    const SqContext merged_jetty = MakeSqContext(9, 0x3000, 0x4000);
    size_t state_a = SIZE_MAX;
    size_t state_b = SIZE_MAX;
    ASSERT_EQ(ResolveQueueStateBindingForTest(manager, merged_jetty, 1, 0, 1, state_a, jetty_maps), ACLSHMEM_SUCCESS);
    EXPECT_EQ(
        ResolveQueueStateBindingForTest(manager, merged_jetty, 1, 1, 1, state_b, jetty_maps), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(QueueStateCount(manager), 0U);
}
#endif

TEST_F(UdmaEndpointExchangeTest, BindsWqAndCqToTheSameQueueState)
{
    UdmaTransportManager manager;
    InstallAclAllocApis();

    JettyStateMaps jetty_maps;
    const SqContext sq_context = MakeSqContext(3, 0x5000, 0x6000);
    size_t state_idx = SIZE_MAX;
    ASSERT_EQ(ResolveQueueStateBindingForTest(manager, sq_context, 1, 0, 1, state_idx, jetty_maps), ACLSHMEM_SUCCESS);

    aclshmemi_udma_wq_ctx_t wq{};
    aclshmemi_udma_cq_ctx_t cq{};
    ASSERT_EQ(BindQueueStateAddrForTest(manager, state_idx, 1, 0, wq, cq), ACLSHMEM_SUCCESS);
    EXPECT_NE(wq.state_addr, 0U);
    EXPECT_EQ(wq.state_addr, cq.state_addr);

    // A state slot that never got a buffer (null entry) must fail instead of binding 0.
    const size_t null_state_idx = AppendNullQueueState(manager);
    aclshmemi_udma_wq_ctx_t bad_wq{};
    aclshmemi_udma_cq_ctx_t bad_cq{};
    EXPECT_EQ(BindQueueStateAddrForTest(manager, null_state_idx, 1, 0, bad_wq, bad_cq), ACLSHMEM_INNER_ERROR);
    EXPECT_EQ(QueueStateCount(manager), 0U);
}

TEST_F(UdmaEndpointExchangeTest, ChannelConfigWrappersReportSymbolUnavailable)
{
    // Soft-loaded symbols stay null on an old libhcomm.so; the wrappers must report that instead
    // of crashing, and HasChannelConfigApis must be false until all four resolve.
    DlHcommApi::gHcommChannelConfigCreate = RecordConfigCreate;
    EXPECT_FALSE(DlHcommApi::HasChannelConfigApis());

    HcommChannelConfig config = nullptr;
    EXPECT_EQ(DlHcommApi::HcommChannelConfigDestroy(config), HCOMM_RESULT_SYMBOL_UNAVAILABLE);
    EXPECT_EQ(
        DlHcommApi::HcommChannelConfigSetInt(config, HCOMM_CHANNEL_CONFIG_TYPE_IS_SHARED_QUEUE, 1U),
        HCOMM_RESULT_SYMBOL_UNAVAILABLE);
    EXPECT_EQ(
        DlHcommApi::HcommChannelCreateWithConfig(nullptr, COMM_ENGINE_AIV, nullptr, 0, nullptr, nullptr),
        HCOMM_RESULT_SYMBOL_UNAVAILABLE);

    InstallChannelConfigApis();
    EXPECT_TRUE(DlHcommApi::HasChannelConfigApis());
    EXPECT_EQ(DlHcommApi::HcommChannelConfigCreate(&config), 0);
    EXPECT_NE(config, nullptr);
    EXPECT_EQ(DlHcommApi::HcommChannelConfigSetInt(config, HCOMM_CHANNEL_CONFIG_TYPE_IS_SHARED_QUEUE, 1U), 0);
}

} // namespace device
} // namespace transport
} // namespace shm
