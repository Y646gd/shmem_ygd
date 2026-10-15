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

// Test-only access override scoped strictly to dl_hcomm_api.h so this fixture can replace
// dynamically loaded HCOMM entry points with local recorders. Keep the undef immediately
// after this include; do not place other headers inside this scope or copy this pattern to
// other tests. Prefer an explicit injection interface if production code adds one later.
#define private public
#include "dl_hcomm_api.h"
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

    void SetUp() override
    {
        saved_handle_ = g_boot_handle;
        saved_mem_reg_ = DlHcommApi::gHcommMemReg;
        saved_mem_unreg_ = DlHcommApi::gHcommMemUnreg;
        saved_endpoint_destroy_ = DlHcommApi::gHcommEndpointDestroy;
        saved_channel_create_ = DlHcommApi::gHcommChannelCreate;
        saved_channel_destroy_ = DlHcommApi::gHcommChannelDestroy;
        g_boot_handle.npes = 3;
        g_boot_handle.allgather = AllGather;
        lengths_.clear();
        fail_call_ = 0;
        mem_reg_endpoints_.clear();
        channel_endpoints_.clear();
        next_handle_ = 1000;
        DlHcommApi::gHcommMemReg = RecordMemReg;
        DlHcommApi::gHcommMemUnreg = SucceedMemUnreg;
        DlHcommApi::gHcommEndpointDestroy = SucceedEndpointDestroy;
        DlHcommApi::gHcommChannelCreate = RecordChannelCreate;
        DlHcommApi::gHcommChannelDestroy = SucceedChannelDestroy;
    }

    void TearDown() override
    {
        DlHcommApi::gHcommMemReg = saved_mem_reg_;
        DlHcommApi::gHcommMemUnreg = saved_mem_unreg_;
        DlHcommApi::gHcommEndpointDestroy = saved_endpoint_destroy_;
        DlHcommApi::gHcommChannelCreate = saved_channel_create_;
        DlHcommApi::gHcommChannelDestroy = saved_channel_destroy_;
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
                output[rank * count + idx].peer_rank = static_cast<uint32_t>(rank);
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

    static HcommResult RecordChannelCreate(
        EndpointHandle endpoint, CommEngine, HcommChannelDesc*, uint32_t count, ChannelHandle* channels)
    {
        for (uint32_t idx = 0; idx < count; ++idx) {
            channel_endpoints_.push_back(endpoint);
            channels[idx] = next_handle_++;
        }
        return 0;
    }

    static void AddEndpoint(UdmaTransportManager& manager, uint32_t eid, uint32_t peer, uintptr_t handle)
    {
        const auto key = std::make_pair(eid, peer);
        EndpointDesc desc{};
        manager.endpoint_handle_map_[eid] = Handle(handle);
        manager.endpoint_local_desc_map_[eid] = desc;
        manager.endpoint_desc_map_[key] = desc;
        manager.endpoint_listen_port_map_[eid] = static_cast<uint16_t>(1000 + handle);
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

    static void AddRemoteDescriptor(
        Exchange& exchange, uint32_t rank, uint32_t index, uint32_t local_rank, uint32_t eid)
    {
        Descriptor& descriptor = exchange.descs[static_cast<size_t>(rank) * exchange.max_count + index];
        descriptor.valid = 1;
        descriptor.peer_rank = local_rank;
        descriptor.eid_index = eid;
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
    inline static std::vector<int> lengths_;
    inline static size_t fail_call_{0};
    inline static std::vector<EndpointHandle> mem_reg_endpoints_;
    inline static std::vector<EndpointHandle> channel_endpoints_;
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
        EXPECT_LE(receive_bytes, 32 * 1024 * 1024);
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
            EXPECT_EQ(desc.peer_rank, rank);
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
        EXPECT_EQ(single.descs[idx].peer_rank, split.descs[idx].peer_rank);
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
    EXPECT_EQ(LogicalEndpointCount(manager), 3U);
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
    AddRemoteDescriptor(exchange, 1, 0, 0, 20);
    AddRemoteDescriptor(exchange, 1, 1, 0, 21);
    AddRemoteDescriptor(exchange, 2, 0, 0, 22);
    AddRemoteDescriptor(exchange, 2, 1, 0, 23);

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

} // namespace device
} // namespace transport
} // namespace shm
