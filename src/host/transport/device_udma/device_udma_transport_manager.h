/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef MF_HYBRID_DEVICE_UDMA_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_UDMA_TRANSPORT_MANAGER_H

#include <map>
#include <array>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

#include <memory>

#include "transport_manager.h"
#include "transport/topo/topo_reader.h"
#include "hcomm_entity_compat.h"
#include "dl_hcomm_def.h"
#include "device_udma_def.h"
#if defined(ACLSHMEM_MSSANITIZER_BUILD)
#include "mstx/mstx_mem_register.h"
#endif

namespace shm {
namespace transport {
namespace device {

// Single point where the relay build switch is turned into a compile-time predicate. Every relay
// vs. direct decision in this class branches on this constant via `if constexpr`, so the rest of
// the header/.cpp stays free of scattered #ifdef blocks.
#if defined(ACLSHMEM_RELAY_SUPPORT)
inline constexpr bool ACLSHMEM_UDMA_RELAY_ENABLED = true;
#else
inline constexpr bool ACLSHMEM_UDMA_RELAY_ENABLED = false;
#endif

class UdmaTransportManager : public TransportManager {
public:
    UdmaTransportManager() noexcept;
    ~UdmaTransportManager() noexcept;
    Result OpenDevice(const TransportOptions& options) override;
    Result CloseDevice() override;
    Result RegisterMemoryRegion(const TransportMemoryRegion& mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    Result Prepare(const HybmTransPrepareOptions& options) override;
    Result Connect() override;
    const void* GetQpInfo() const override;
    TransportDeviceInfo GetDeviceInfo() const override;
    Result GetQpNetLayer(
        data_op_engine_type_t engine, uint32_t peer, uint32_t qpIdx, uint32_t& netLayer) const override;
    Result QueryMemoryKey(uint64_t /*addr*/, TransportMemoryKey& /*key*/) override { return ACLSHMEM_SUCCESS; }
    Result ParseMemoryKey(const TransportMemoryKey& /*key*/, uint64_t& /*addr*/, uint64_t& /*size*/) override
    {
        return ACLSHMEM_SUCCESS;
    }
    Result AsyncConnect() override;
    Result WaitForConnected(int64_t /*timeoutNs*/) override { return ACLSHMEM_SUCCESS; }
    Result UpdateRankOptions(const HybmTransPrepareOptions& /*options*/) override { return ACLSHMEM_SUCCESS; }
    const std::string& GetNic() const override
    {
        static const std::string empty_nic;
        return empty_nic;
    }

private:
    friend class UdmaEndpointExchangeTest;

    // One rank's local HCOMM endpoint descriptor as exchanged via allgather. `valid` distinguishes
    // real entries from the zero padding used to make every rank contribute max_count slots.
    struct ExchangedEndpointDesc {
        uint32_t eid_index{0};
        uint32_t peer_rank{0};
        uint32_t valid{0};
        uint16_t listen_port{0};
        uint16_t reserved{0};
        EndpointDesc desc{};
    };
    // Result of the endpoint-descriptor allgather. `descs` is a flat [rank * max_count + idx] table.
    struct EndpointExchange {
        std::vector<uint32_t> counts;             // per-rank endpoint count
        uint32_t max_count{0};                    // max endpoint count across ranks (stride of descs)
        std::vector<ExchangedEndpointDesc> descs; // exchanged descriptors, indexed [rank * max_count + idx]
    };
    static bool GetEndpointExchangeSize(uint32_t rank_count, uint32_t max_count, size_t& descriptor_count);
    static uint32_t GetEndpointChunkCount(uint32_t rank_count);
    static Result GatherEndpointChunks(
        const std::vector<ExchangedEndpointDesc>& local_endpoints, uint32_t rank_count, uint32_t chunk_count,
        EndpointExchange& exchange);
    // Channels created during Connect, kept in parallel arrays. handles[i] feeds udma info slot
    // slots[i] and physically targets dst_pes[i]. Direct: slots[i] == dst_pes[i] == peer;
    // relay: slots[i] == dst_pes[i] * rank_count_ + relay_pe.
    struct ChannelBuildState {
        std::vector<ChannelHandle> handles;
        std::vector<uint32_t> slots;
        std::vector<uint32_t> dst_pes;
        std::vector<uint32_t> qp_indices;
        // HCOMM may retain channelName while connecting asynchronously. BuildChannels reserves
        // the complete direct multi-QP name count before insertion so c_str() addresses stay stable.
        std::vector<std::string> channel_names;
    };
    struct QpRoute {
        uint32_t local_eid_index{0};
        uint32_t remote_eid_index{0};
        NetType net_type{};
        uint32_t net_layer{};
        std::string net_instance_id{};
        std::string plane_id{};
        size_t port_count{};
    };
    struct PeerRouteInfo {
        QpRoute default_route{};
        std::vector<QpRoute> qp_routes{};
    };
    bool CreateEndpoint(uint32_t eid_index, uint32_t peer_rank, const std::array<uint8_t, 16>& target_eid_raw);
    Result PrepareOpenDevice(uint32_t rank_count);
    // Allgather the local HCOMM endpoint descriptors into `exchange`.
    Result ExchangeEndpointDescriptors(EndpointExchange& exchange) const;
    // Locate dst_pe's exchanged endpoint descriptor on remote_eid_index, or nullptr if absent.
    const ExchangedEndpointDesc* FindRemoteEndpointDesc(
        const EndpointExchange& exchange, uint32_t dst_pe, uint32_t remote_eid_index) const;
    // Batch-create channels egressing on local_eid_index toward dst_pe's endpoint on
    // remote_eid_index, and append them to `state` against udma info `slot`.
    Result CreateChannelsForSlot(
        const EndpointExchange& exchange, uint32_t local_eid_index, uint32_t remote_eid_index, uint32_t dst_pe,
        uint32_t slot, uint32_t first_qp_idx, uint32_t channel_num, ChannelBuildState& state);
    // Create all channels for this build: dispatches to the direct or relay variant based on
    // ACLSHMEM_UDMA_RELAY_ENABLED. On failure the caller cleans up state.handles.
    Result BuildChannels(const EndpointExchange& exchange, ChannelBuildState& state);
    // Direct build: slot == peer, and each peer/QP route creates one HCOMM channel.
    Result BuildDirectChannels(const EndpointExchange& exchange, ChannelBuildState& state);
    // One channel per meaningful (actual_pe, relay_pe) slot (relay build). Only reached when
    // ACLSHMEM_UDMA_RELAY_ENABLED. Channels are created non-blocking; readiness is polled once
    // afterwards via WaitHcommChannelReady, so no per-wave synchronization is needed.
    Result BuildRelayChannels(const EndpointExchange& exchange, ChannelBuildState& state);
    // Number of UDMA info slots. Direct (OFF) build: one slot per target peer (slot == peer),
    // slot_count == rank_count_. Relay (ON) build: one slot per (actual_pe, relay_pe) pair,
    // slot_count == rank_count_ * rank_count_, addressed by actual_pe * rank_count_ + relay_pe.
    uint64_t SlotCount() const;
    // channel_slots[i] is the UDMA info slot the i-th created channel feeds. Direct: slot == peer;
    // relay: slot == actual_pe * rank_count_ + relay_pe. channel_dst_pes[i] is the actual
    // destination PE of that channel (used for validation only).
    Result BuildUdmaInfo(
        const std::vector<uint64_t>& channel_ptrs, const std::vector<uint32_t>& channel_slots,
        const std::vector<uint32_t>& channel_dst_pes, const std::vector<uint32_t>& channel_qp_indices);
    Result ReadChannelContexts(
        const std::vector<uint64_t>& channel_ptrs, const std::vector<uint32_t>& channel_slots,
        const std::vector<uint32_t>& channel_dst_pes, const std::vector<uint32_t>& channel_qp_indices,
        std::vector<SqContext>& sq_contexts_by_qp, std::vector<CqContext>& cq_contexts_by_qp,
        std::vector<RegedBufferEntity>& remote_buffers_by_qp, std::vector<bool>& qp_valid) const;
    Result PrepareUdmaInfoBuffers(std::vector<uint8_t>& eid_table_host);
    Result InitHostUdmaInfo(std::vector<uint8_t>& udma_info_buffer, aclshmemi_aiv_udma_info_t*& copy_info);
    Result FillHostUdmaInfo(
        const std::vector<SqContext>& sq_contexts_by_qp, const std::vector<CqContext>& cq_contexts_by_qp,
        const std::vector<RegedBufferEntity>& remote_buffers_by_qp, const std::vector<bool>& qp_valid,
        std::vector<uint8_t>& eid_table_host, aclshmemi_aiv_udma_info_t& copy_info);
    Result CopyEidTableToDevice(const std::vector<uint8_t>& eid_table_host);
    Result CopyUdmaInfoToDevice(std::vector<uint8_t>& udma_info_buffer, aclshmemi_aiv_udma_info_t& copy_info);
    Result ReserveScratchBuffers();
    void FillWqCtx(const SqContext& sq_context, uint32_t dst_pe, aclshmemi_udma_wq_ctx_t& dst_wq) const;
    void FillCqCtx(const CqContext& cq_context, aclshmemi_udma_cq_ctx_t& dst_cq) const;
    void FillMemInfo(
        const SqContext& sq_context, const RegedBufferEntity& remote_buffer, aclshmemi_ubmem_info_t& dst_mem) const;
    void PrintHostUdmaInfo(const aclshmemi_aiv_udma_info_t& host_info) const;
    std::vector<HcommMemHandle> CollectChannelMemHandles(uint32_t eid_index) const;
#if defined(ACLSHMEM_MSSANITIZER_BUILD)
    // mssanitizer: report/withdraw the URMA-allocated (non-rtMalloc) SQ/CQ rings and doorbells
    // for every active slot/QP context. Compiled out of normal builds.
    void RegisterUdmaQpSanitizerRegions(const aclshmemi_aiv_udma_info_t& copy_info);
    void UnregisterUdmaQpSanitizerRegions();
#endif
    void FreeDeviceInfo();
    void DestroyChannels();
    Result CheckPrepareOptions(const HybmTransPrepareOptions& options);
    void CleanupResources();
    // Relay-only helper (only reached when ACLSHMEM_UDMA_RELAY_ENABLED == true).
    // Resolve (local egress EID, remote target EID) for relay slot (actual_pe, relay_pe).
    // skip=true for the meaningless diagonal (actual == relay, actual != me).
    Result ResolveRelaySlotRoute(
        uint32_t actual_pe, uint32_t relay_pe, bool& skip, uint32_t& local_eid, uint32_t& remote_eid) const;

private:
    uint32_t rank_id_{0};
    uint32_t rank_count_{1};
    uint32_t qp_num_{1};
    std::map<uint32_t, uint32_t> net_layer_policy_{};
    bool multi_link_enabled_{false};
    std::unique_ptr<TopologyContext> owned_topology_context_;
    const TopologyContext* topology_context_{nullptr};
    uint32_t phy_id_{0};
    hybm_role_type role_{hybm_role_type::HYBM_ROLE_PEER};
    // peerRankId -> route state. default_route is the first resolved route and is used by
    // relay/legacy slots; qp_routes is used by direct multi-QP slots.
    std::map<uint32_t, PeerRouteInfo> peer_routes_;
    // N x N routing matrix: [rank * rank_count_ + peer] = local-port eid_index `rank` uses to reach
    // `peer`. Only read on the relay path to resolve each (actual_pe, relay_pe) slot's target EID.
    std::vector<int32_t> all_local_routes_;
    // HCOMM endpoints and memory registrations are local-EID resources and are shared by all peers using that EID.
    // endpoint_desc_map_ retains the logical (eid_index, peer_rank) bindings required by descriptor exchange.
    std::map<std::pair<uint32_t, uint32_t>, EndpointDesc> endpoint_desc_map_;
    std::map<uint32_t, EndpointDesc> endpoint_local_desc_map_;              // eid_index -> endpoint desc
    std::map<uint32_t, EndpointHandle> endpoint_handle_map_;                // eid_index -> handle
    std::map<uint32_t, uint16_t> endpoint_listen_port_map_;                 // eid_index -> port
    std::map<uint64_t, std::map<uint32_t, HcommMemHandle>> mem_record_map_; // addr -> eid_index -> handle
    // Registration geometry is kept separately from the opaque HCOMM handles. The current UDMA
    // data-plane ABI carries one memory descriptor per channel slot, so Connect requires this map
    // to contain exactly one entry and validates that the peer descriptor covers the same heap.
    std::map<uint64_t, uint64_t> mem_region_size_map_; // addr -> registered bytes
    std::vector<ChannelHandle> channel_handles_;
    // The control plane fills a contiguous aclshmemi_aiv_udma_info_t blob using the legacy
    // (jetty-manager) layout so the data plane consumes it unchanged. The per-peer
    // amo / remote-EID scratch buffers are allocated separately, mirroring
    // the original DeviceJettyManager allocation scheme.
    void* udma_info_dev_{nullptr}; // device pointer to the contiguous aclshmemi_aiv_udma_info_t blob
    uint64_t udma_info_size_{0};   // byte size of the contiguous blob
    // device pointer to uint8_t[SlotCount()][qp_num_][16] remote EID raw, indexed by slot/QP.
    // Direct build: SlotCount() == rank_count_, slot == pe. Relay build: SlotCount() == rank_count_^2,
    // slot == actual_pe * rank_count_ + relay_pe, qp == 0.
    void* eid_dev_{nullptr};
    // per-peer uint64_t AMO scratch device buffers, sized rank_count_ and indexed by the actual
    // destination pe in both builds (relay recovers dst_pe = slot / rank_count_).
    std::vector<void*> amo_dev_list_;
#if defined(ACLSHMEM_MSSANITIZER_BUILD)
    // Owns the mssanitizer registrar only in instrumented builds.
    std::unique_ptr<shm::mstx_mem_register_base> mstx_reg_ptr_;
    bool udma_qp_regions_registered_{false};
#endif
};
} // namespace device
} // namespace transport
} // namespace shm

#endif // MF_HYBRID_DEVICE_UDMA_TRANSPORT_MANAGER_H
