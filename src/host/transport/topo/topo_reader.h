/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef MF_HYBRID_TRANSPORT_TOPO_READER_H
#define MF_HYBRID_TRANSPORT_TOPO_READER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <unordered_map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "host/shmem_host_def.h"
#include "sotre_net.h"

struct aclshmemi_bootstrap_handle;

namespace shm {
namespace transport {

constexpr std::size_t URMA_EID_RAW_SIZE = 16;
constexpr std::size_t URMA_EID_HEX_SIZE = URMA_EID_RAW_SIZE * 2;
constexpr uint32_t URMA_EID_IPV4_PREFIX = 0x0000;

using EidData = std::array<uint8_t, URMA_EID_RAW_SIZE>;
using EidPort = std::string;
using EidIndex = uint32_t;

enum class NetType { Mesh = 0, Clos = 1, TopoFileDesc = 2 };

enum class TopoFabric { Unknown = 0, Mesh = 1, Clos = 2 };

enum class RankAddrType { EID = 0, IPV4 = 1, IPV6 = 2 };

struct LevelInfo {
    uint32_t netLayer{};
    std::string netInstanceId{};
    NetType netType{};
    std::string netAttr{};
};

struct RankAddr {
    RankAddrType addrType{};
    EidData eidData{};
    std::string planeId{};

    std::vector<EidPort> ports{};
    std::shared_ptr<LevelInfo> levelInfo{};
};

struct RootInfo {
    std::string topoFilePath{};
    uint32_t deviceId{};
    uint32_t localId{};
    uint32_t totalEidCount{};

    std::vector<std::shared_ptr<LevelInfo>> levels{};
    std::map<EidIndex, std::shared_ptr<RankAddr>> eidIndexToRankAddr{};
    std::map<EidPort, std::shared_ptr<RankAddr>> portsToRankAddr{};
};

struct MeshTopoEdge {
    uint32_t localA{};
    uint32_t localB{};
    std::vector<EidPort> localAPorts{};
    std::vector<EidPort> localBPorts{};
};

struct ClosTopoEdge {
    uint32_t netLayer{};
    uint32_t topoInstanceId{};

    uint32_t localA{};
    std::vector<EidPort> ports{};
};

struct TopoInfo {
    std::vector<MeshTopoEdge> meshTopoEdges{};
    std::vector<ClosTopoEdge> closTopoEdges{};
    std::unordered_map<EidPort, uint8_t> portFabricMask{};
    std::unordered_map<uint64_t, std::size_t> meshEdgeByLocalPair{};
    std::unordered_map<uint32_t, std::vector<std::size_t>> closEdgeIndicesByLocalA{};
};

// Per-rank-addr view exchanged across ranks during PrepareOpenDevice. One SyncEndpoint
// is emitted for every rank_addr on every level. Two ranks are peers on a given CLOS
// plane through the driver topo file when rootinfo uses TOPO_FILE_DESC; Mesh and CLOS are
// selected by rootinfo netType for explicit levels, or by topo_type for TOPO_FILE_DESC.
// eidData is intentionally NOT carried: the local eid raw is fetched from the local rootInfo by eidIndex.
struct SyncEndpoint {
    uint32_t netLayer{};
    std::string netInstanceId{};
    NetType netType{};
    TopoFabric fabric{TopoFabric::Unknown};
    std::string planeId{};
    std::vector<EidPort> ports{}; // used to reverse-map a peer port to its eidIndex
    uint32_t eidIndex{};
    RankAddrType addrType{RankAddrType::EID};
};

inline void to_json(nlohmann::json& json, const SyncEndpoint& endpoint)
{
    json = nlohmann::json{
        {"netLayer", endpoint.netLayer},
        {"netInstanceId", endpoint.netInstanceId},
        {"netType", static_cast<uint32_t>(endpoint.netType)},
        {"fabric", static_cast<uint32_t>(endpoint.fabric)},
        {"addrType", static_cast<uint32_t>(endpoint.addrType)},
        {"planeId", endpoint.planeId},
        {"ports", endpoint.ports},
        {"eidIndex", endpoint.eidIndex},
    };
}

inline void from_json(const nlohmann::json& json, SyncEndpoint& endpoint)
{
    json.at("netLayer").get_to(endpoint.netLayer);
    json.at("netInstanceId").get_to(endpoint.netInstanceId);
    if (json.contains("netType")) {
        uint32_t netType = 0;
        json.at("netType").get_to(netType);
        endpoint.netType = static_cast<NetType>(netType);
    } else {
        endpoint.netType = (endpoint.netLayer == 0) ? NetType::Mesh : NetType::Clos;
    }
    if (json.contains("fabric")) {
        uint32_t fabric = 0;
        json.at("fabric").get_to(fabric);
        endpoint.fabric = static_cast<TopoFabric>(fabric);
    } else if (endpoint.netType == NetType::Mesh) {
        endpoint.fabric = TopoFabric::Mesh;
    } else if (endpoint.netType == NetType::Clos) {
        endpoint.fabric = TopoFabric::Clos;
    } else {
        endpoint.fabric = TopoFabric::Unknown;
    }
    if (json.contains("addrType")) {
        uint32_t addrType = 0;
        json.at("addrType").get_to(addrType);
        endpoint.addrType = static_cast<RankAddrType>(addrType);
    } else {
        endpoint.addrType = RankAddrType::EID;
    }
    json.at("planeId").get_to(endpoint.planeId);
    json.at("ports").get_to(endpoint.ports);
    json.at("eidIndex").get_to(endpoint.eidIndex);
}

// Immutable discovery shared by instance queries and UDMA route selection.
struct TopologyContext {
    uint32_t physical_device_id{0};
    uint32_t max_eid_slot_count{0};
    uint32_t my_pe{0};
    RootInfo root_info;
    TopoInfo topo_info;
    std::vector<uint32_t> rank_to_local_id;
    std::vector<std::vector<SyncEndpoint>> rank_to_sync_endpoints;

    int Initialize(int32_t user_device_id, uint32_t rank, uint32_t npes,
                   aclshmemi_bootstrap_handle* bootstrap);
    static std::vector<SyncEndpoint> BuildLocalEndpoints(const RootInfo& root, const TopoInfo& topo);
    // No heap allocation: even an allocation failure must agree before the next exchange.
    static int Agree(int status, uint32_t npes, aclshmemi_bootstrap_handle* bootstrap);
};

struct EidRoute {
    uint32_t localEidIndex{};
    EidData localEidRaw{};
    uint32_t remoteEidIndex{};
    NetType netType{};
    uint32_t netLayer{};
    std::string netInstanceId{};
    std::string planeId{};
    size_t portCount{};
};

class EidConverter {
public:
    static bool Convert(RankAddrType addrType, const nlohmann::json& addr, EidData& raw);

private:
    static bool FromPlainEid(RankAddrType addrType, const nlohmann::json& addr, EidData& raw);
    static bool FromIpv4(RankAddrType addrType, const nlohmann::json& addr, EidData& raw);
    static bool FromIpv6(RankAddrType addrType, const nlohmann::json& addr, EidData& raw);
};

class TopoReader {
public:
#ifdef ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO
    // TEMP_TOPOLOGY_CPP_JSON_STUB: compile-time file override for portable C++ tests.
    static constexpr const char* ROOTINFO_PATH = ACLSHMEM_TOPOLOGY_JSON_STUB_ROOTINFO;
#else
    static constexpr const char* ROOTINFO_PATH = "/etc/hccl_rootinfo.json";
#endif
    static bool ParseRootInfo(uint32_t phyId, RootInfo& out);
#if defined(ACLSHMEM_UNIT_TEST) && defined(ACLSHMEM_SOC_950)
    // TEMP_TOPOLOGY_JSON_STUB: bypass device/file discovery, retain production parsing.
    static bool ParseRootInfoJsonForTest(const nlohmann::json& input, uint32_t phyId, RootInfo& out)
    {
        out = RootInfo{};
        if (ParseRootInfoJson(input, phyId, out)) return true;
        out = RootInfo{};
        return false;
    }
#endif

    static bool ParseTopoInfo(const std::string& path, TopoInfo& out);
    static bool GetLocalId(const RootInfo& root, uint32_t deviceId, uint32_t& localId);

    static bool GetEidCount(const RootInfo& root, uint32_t& count);

    static bool ParseRdmaNetAddr(uint32_t phyId, net_addr_t& outIp);

private:
    static bool LoadRootInfoJson(uint32_t phyId, nlohmann::json& out);
    static bool ParseRootInfoJson(const nlohmann::json& rootInfoJson, uint32_t phyId, RootInfo& out);
    static bool ParseLevelInfo(const nlohmann::json& levelJson, LevelInfo& out);
    static bool ParseRankAddr(
        const nlohmann::json& rankAddrJson, uint32_t localId, uint32_t eidIndex,
        const std::shared_ptr<LevelInfo>& levelInfo, RankAddr& out);
    static bool ParseUint(const nlohmann::json& jsonValue, uint32_t& value);
};

class TopoQuerier {
public:
    explicit TopoQuerier(
        const RootInfo& r, const TopoInfo& t, uint32_t myRank, const std::vector<uint32_t>& rankToLocalId,
        const std::vector<std::vector<SyncEndpoint>>& rankIdxToSyncEndpoint,
        const std::map<uint32_t, uint32_t>& netLayerPolicy = {}, bool multiLinkEnabled = false,
        const std::vector<uint32_t>* rankToServerId = nullptr)
        : rootInfo_(r),
          topoInfo_(t),
          myRank_(myRank),
          rankToLocalId_(rankToLocalId),
          rankIdxToSyncEndpoint_(rankIdxToSyncEndpoint),
          netLayerPolicy_(netLayerPolicy),
          multiLinkEnabled_(multiLinkEnabled),
          rankToServerId_(rankToServerId)
    {}

    // Physical inventory, independent of route selection, QP count and engine enablement.
    void GetReachableNetLayers(uint32_t targetRank, std::vector<uint32_t>& layers) const;

    Result GetEidRoutes(uint32_t targetRank, uint32_t routeCount, std::vector<EidRoute>& routes);
    bool GetEidRoute(uint32_t targetRank, uint32_t& localEidIndex, EidData& localEidRaw, uint32_t& remoteEidIndex);

private:
    Result ResolveEidRoutes(uint32_t targetRank, uint32_t routeCount, std::vector<EidRoute>& routes);
    bool GetEidRouteMesh1D(uint32_t targetRank, EidRoute& route);

    const RootInfo& rootInfo_;
    const TopoInfo& topoInfo_;
    uint32_t myRank_;

    const std::vector<uint32_t>& rankToLocalId_;
    const std::vector<std::vector<SyncEndpoint>>& rankIdxToSyncEndpoint_;
    std::map<uint32_t, uint32_t> netLayerPolicy_;
    bool multiLinkEnabled_{false};
    const std::vector<uint32_t>* rankToServerId_{nullptr};
};

} // namespace transport
} // namespace shm

#endif // MF_HYBRID_TRANSPORT_TOPO_READER_H
