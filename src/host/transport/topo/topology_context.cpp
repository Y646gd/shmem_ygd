/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "topo_reader.h"
#include "dl_acl_api.h"
#include "utils/shmemi_host_types.h"
#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace shm {
namespace transport {
namespace {
bool PortInMeshTopo(const TopoInfo& topoInfo, const EidPort& port)
{
    for (const auto& edge : topoInfo.meshTopoEdges) {
        if (std::find(edge.localAPorts.begin(), edge.localAPorts.end(), port) != edge.localAPorts.end() ||
            std::find(edge.localBPorts.begin(), edge.localBPorts.end(), port) != edge.localBPorts.end()) {
            return true;
        }
    }
    return false;
}

bool PortInClosTopo(const TopoInfo& topoInfo, const EidPort& port)
{
    for (const auto& edge : topoInfo.closTopoEdges) {
        if (std::find(edge.ports.begin(), edge.ports.end(), port) != edge.ports.end()) {
            return true;
        }
    }
    return false;
}

TopoFabric ClassifyEndpointFabric(const TopoInfo& topoInfo, const std::vector<EidPort>& ports)
{
    bool meshMatched = false;
    bool closMatched = false;
    for (const auto& port : ports) {
        meshMatched = meshMatched || PortInMeshTopo(topoInfo, port);
        closMatched = closMatched || PortInClosTopo(topoInfo, port);
        if (meshMatched && closMatched) {
            return TopoFabric::Unknown;
        }
    }
    if (closMatched) {
        return TopoFabric::Clos;
    }
    if (meshMatched) {
        return TopoFabric::Mesh;
    }
    return TopoFabric::Unknown;
}

TopoFabric ResolveEndpointFabric(NetType netType, const TopoInfo& topoInfo, const std::vector<EidPort>& ports)
{
    // An explicit rootinfo network type is authoritative. In particular, an
    // aggregated PoD CLOS address can contain ports which also appear in a
    // Mesh topology edge; inferring its fabric from the complete port set
    // would incorrectly turn that explicit CLOS endpoint into Unknown.
    if (netType == NetType::Mesh) {
        return TopoFabric::Mesh;
    }
    if (netType == NetType::Clos) {
        return TopoFabric::Clos;
    }
    // TOPO_FILE_DESC intentionally relies on the driver topology to identify
    // the fabric because its rootinfo level describes a merged topology.
    return ClassifyEndpointFabric(topoInfo, ports);
}


template <typename Work>
int Prepare(Work work)
{
    try {
        return work();
    } catch (const std::bad_alloc&) {
        return ACLSHMEM_MALLOC_FAILED;
    } catch (const std::exception&) {
        return ACLSHMEM_INNER_ERROR;
    }
}
} // namespace

int TopologyContext::Agree(int status, uint32_t npes, aclshmemi_bootstrap_handle* bootstrap)
{
    if (bootstrap == nullptr || bootstrap->allgather == nullptr || npes == 0 || npes > ACLSHMEM_MAX_PES) {
        return ACLSHMEM_NOT_INITED;
    }
    std::array<int, ACLSHMEM_MAX_PES> statuses{};
    if (bootstrap->allgather(&status, statuses.data(), sizeof(status), bootstrap) != 0) {
        return ACLSHMEM_BOOTSTRAP_ERROR;
    }
    for (uint32_t pe = 0; pe < npes; ++pe) {
        if (statuses[pe] != ACLSHMEM_SUCCESS) {
            return statuses[pe];
        }
    }
    return ACLSHMEM_SUCCESS;
}

std::vector<SyncEndpoint> TopologyContext::BuildLocalEndpoints(const RootInfo& root, const TopoInfo& topo)
{
    std::vector<SyncEndpoint> endpoints;
    for (const auto& item : root.eidIndexToRankAddr) {
        const auto& addr = item.second;
        if (addr == nullptr || addr->levelInfo == nullptr) {
            continue;
        }
        SyncEndpoint endpoint;
        endpoint.netLayer = addr->levelInfo->netLayer;
        endpoint.netInstanceId = addr->levelInfo->netInstanceId;
        endpoint.netType = addr->levelInfo->netType;
        endpoint.fabric = ResolveEndpointFabric(endpoint.netType, topo, addr->ports);
        endpoint.addrType = addr->addrType;
        endpoint.planeId = addr->planeId;
        endpoint.ports = addr->ports;
        endpoint.eidIndex = item.first;
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

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
} // namespace transport
} // namespace shm
