/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>
#include <tuple>
#include <vector>

#if defined(ACLSHMEMI_RDMA_K_BACKEND_HNS_1825)
#include "rootinfo/aclshmemi_virtual_topology_root_info.h"
#endif
#include "rootinfo/topo_addr_info.h"
#include "shmemi_file_util.h"
#include "shmemi_host_common.h"
#include "topo_reader.h"

namespace shm {
namespace transport {

namespace {

constexpr uint8_t FABRIC_MESH_MASK = 1U;
constexpr uint8_t FABRIC_CLOS_MASK = 2U;

uint64_t MakeLocalPairKey(uint32_t lhs, uint32_t rhs)
{
    if (lhs > rhs) {
        std::swap(lhs, rhs);
    }
    return (static_cast<uint64_t>(lhs) << 32U) | static_cast<uint64_t>(rhs);
}

void BuildTopoIndexes(TopoInfo& topoInfo);

bool CheckTopoFilePath(const std::string& path, std::string& realPath)
{
    realPath = path;
    if (utils::FileUtil::IsSymlink(realPath) || !utils::FileUtil::Realpath(realPath) ||
        !utils::FileUtil::IsFile(realPath) || !utils::FileUtil::CheckFileSize(realPath)) {
        SHM_LOG_ERROR("Topo file path check failed: " << path);
        return false;
    }
    return true;
}

bool NetTypeFromString(const std::string& netType, NetType& out)
{
    if (netType == "TOPO_FILE_DESC") {
        out = NetType::TopoFileDesc;
        return true;
    }
    if (netType == "MESH") {
        out = NetType::Mesh;
        return true;
    }
    if (netType == "CLOS") {
        out = NetType::Clos;
        return true;
    }
    return false;
}

// addrType defaults to EID when the field is absent; IPV4 / IPV6 select the dotted-string
// converters. Matching is case-insensitive: the caller upper-cases before calling.
bool RankAddrTypeFromString(const std::string& addrType, RankAddrType& out)
{
    if (addrType == "EID") {
        out = RankAddrType::EID;
        return true;
    }
    if (addrType == "IPV4") {
        out = RankAddrType::IPV4;
        return true;
    }
    if (addrType == "IPV6") {
        out = RankAddrType::IPV6;
        return true;
    }
    return false;
}

const char* AddrTypeName(RankAddrType addrType)
{
    switch (addrType) {
        case RankAddrType::EID:
            return "EID";
        case RankAddrType::IPV4:
            return "IPV4";
        case RankAddrType::IPV6:
            return "IPV6";
        default:
            return "UNKNOWN";
    }
}

// TryGet: extract a typed value from a json node. Returns true and writes out only when
// the node holds a compatible value; never logs. One overload per supported field type.
bool TryGet(const nlohmann::json& value, std::string& out)
{
    if (!value.is_string()) {
        return false;
    }
    out = value.get<std::string>();
    return true;
}

bool TryGet(const nlohmann::json& value, uint32_t& out)
{
    try {
        if (value.is_string()) {
            out = static_cast<uint32_t>(std::stoul(value.get<std::string>()));
            return true;
        }
        if (value.is_number()) {
            out = value.get<uint32_t>();
            return true;
        }
    } catch (const std::exception&) {
        // fall through to the failure return below
    }
    return false;
}

bool TryGet(const nlohmann::json& value, std::vector<std::string>& out)
{
    if (!value.is_array()) {
        return false;
    }
    out.clear();
    for (const auto& element : value) {
        if (element.is_string()) {
            out.push_back(element.get<std::string>());
        }
    }
    return true;
}

// GetRequiredField: field must exist and hold a value of type T, else log and fail.
// context carries caller-side locators (localId/phyId/eidIndex) for diagnostics.
template <typename T>
bool GetRequiredField(const nlohmann::json& obj, const char* key, T& out, const std::string& context = "")
{
    const auto item = obj.find(key);
    if (item == obj.end() || !TryGet(*item, out)) {
        SHM_LOG_ERROR("Rootinfo/Topo: missing or invalid field '" << key << "'. " << context);
        return false;
    }
    return true;
}

// GetOptionalField: write out only when the field exists and matches type T, else leave
// out untouched. Silent by design.
template <typename T>
bool GetOptionalField(const nlohmann::json& obj, const char* key, T& out)
{
    const auto item = obj.find(key);
    return item != obj.end() && TryGet(*item, out);
}

// GetRequiredArray: returns a pointer to the array node for iteration, or nullptr (with a
// log) when the field is missing, not an array, or empty while requireNonEmpty is set.
const nlohmann::json* GetRequiredArray(
    const nlohmann::json& obj, const char* key, bool requireNonEmpty, const std::string& context = "")
{
    const auto item = obj.find(key);
    if (item == obj.end() || !item->is_array() || (requireNonEmpty && item->empty())) {
        SHM_LOG_ERROR("Rootinfo/Topo: missing or invalid array '" << key << "'. " << context);
        return nullptr;
    }
    return &(*item);
}

} // namespace

bool TopoReader::LoadRootInfoJson(uint32_t phyId, nlohmann::json& out)
{
    bool shouldFallback = false;
    std::ifstream rootInfoFile(ROOTINFO_PATH);
    if (!rootInfoFile.is_open()) {
        SHM_LOG_WARN(
            "Rootinfo file not found at " << ROOTINFO_PATH << ", fallback to generated rootinfo for phyId " << phyId);
        shouldFallback = true;
    } else {
        try {
            rootInfoFile >> out;
        } catch (const std::exception& ex) {
            SHM_LOG_ERROR("Parse rootinfo file failed, the path is " << ROOTINFO_PATH << ", error: " << ex.what());
            SHM_LOG_WARN("Rootinfo file parse failed, fallback to generated rootinfo for phyId " << phyId);
            shouldFallback = true;
        }
    }

    if (!shouldFallback) {
        SHM_LOG_INFO("Load rootinfo from file " << ROOTINFO_PATH);
        return true;
    }

    std::string generatedRootInfo;
#if defined(ACLSHMEMI_RDMA_K_BACKEND_HNS_1825)
    auto virtualTopologyRootInfo =
        shm::topo::aclshmemi_generate_virtual_topology_root_info_json(static_cast<int>(phyId));
    if (!virtualTopologyRootInfo) {
        SHM_LOG_ERROR("Failed to generate virtual-topology rootinfo for phyId " << phyId);
        return false;
    }
    generatedRootInfo = std::move(*virtualTopologyRootInfo);
#else
    size_t rootInfoSize = 0;
    int ret = topo_addr_info_get_size(static_cast<int>(phyId), &rootInfoSize);
    if (ret != 0 || rootInfoSize == 0) {
        SHM_LOG_ERROR(
            "Failed to get generated rootinfo size for phyId " << phyId << ", ret = " << ret
                                                               << ", size = " << rootInfoSize);
        return false;
    }
    SHM_LOG_INFO("Generated rootinfo size for phyId " << phyId << " is " << rootInfoSize);

    std::vector<char> rootInfoBuffer(rootInfoSize + 1, '\0');
    size_t actualSize = rootInfoSize;
    ret = topo_addr_info_get(static_cast<int>(phyId), rootInfoBuffer.data(), &actualSize);
    if (ret != 0 || actualSize == 0) {
        SHM_LOG_ERROR(
            "Failed to get generated rootinfo for phyId " << phyId << ", ret = " << ret
                                                          << ", actualSize = " << actualSize);
        return false;
    }
    if (actualSize > rootInfoBuffer.size() - 1) {
        SHM_LOG_ERROR(
            "Generated rootinfo size overflow, actualSize " << actualSize << ", capacity " << rootInfoBuffer.size());
        return false;
    }
    generatedRootInfo.assign(rootInfoBuffer.data(), actualSize);
#endif

    try {
        out = nlohmann::json::parse(generatedRootInfo);
#ifdef DEBUG_MODE
        SHM_LOG_DEBUG("Generated rootinfo json for phyId " << phyId << ":\n" << out.dump(2));
#endif
    } catch (const std::exception& ex) {
        SHM_LOG_ERROR("Failed to parse generated rootinfo json for phyId " << phyId << ", error: " << ex.what());
        return false;
    }

    SHM_LOG_INFO("Use generated rootinfo fallback for phyId " << phyId);
    return true;
}

bool TopoReader::ParseRootInfo(uint32_t phyId, RootInfo& out)
{
    out = RootInfo{};
    nlohmann::json rootInfoJson;
    if (!LoadRootInfoJson(phyId, rootInfoJson)) {
        SHM_LOG_ERROR("Failed to load rootinfo for phyId " << phyId);
        return false;
    }

    if (!ParseRootInfoJson(rootInfoJson, phyId, out)) {
        out = RootInfo{};
        SHM_LOG_ERROR("Rootinfo content is unusable for phyId " << phyId);
        return false;
    }
    return true;
}

bool TopoReader::ParseLevelInfo(const nlohmann::json& levelJson, LevelInfo& out)
{
    std::string netType;
    if (!GetRequiredField(levelJson, "net_type", netType)) {
        return false;
    }
    if (!NetTypeFromString(netType, out.netType)) {
        SHM_LOG_ERROR("Rootinfo: unsupported net_type " << netType);
        return false;
    }

    out.netLayer = 0;
    if (!GetRequiredField(levelJson, "net_layer", out.netLayer) ||
        !GetRequiredField(levelJson, "net_instance_id", out.netInstanceId)) {
        return false;
    }
    GetOptionalField(levelJson, "net_attr", out.netAttr);
    return true;
}

bool TopoReader::ParseRankAddr(
    const nlohmann::json& rankAddrJson, uint32_t localId, uint32_t eidIndex,
    const std::shared_ptr<LevelInfo>& levelInfo, RankAddr& out)
{
    const std::string context = "localId " + std::to_string(localId) + ", eidIndex " + std::to_string(eidIndex);
    if (!rankAddrJson.contains("addr")) {
        SHM_LOG_ERROR("Rootinfo rank_addr entry missing addr, " << context);
        return false;
    }

    out.levelInfo = levelInfo;

    std::string addrTypeStr = "EID";
    if (rankAddrJson.contains("addr_type") && !TryGet(rankAddrJson["addr_type"], addrTypeStr)) {
        SHM_LOG_ERROR("Rootinfo addr_type format is unsupported, " << context);
        return false;
    }
    std::transform(addrTypeStr.begin(), addrTypeStr.end(), addrTypeStr.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    if (!RankAddrTypeFromString(addrTypeStr, out.addrType)) {
        SHM_LOG_ERROR("Rootinfo addr_type is unsupported: " << addrTypeStr << ", " << context);
        return false;
    }

    if (!EidConverter::Convert(out.addrType, rankAddrJson["addr"], out.eidData)) {
        SHM_LOG_ERROR("Rootinfo: failed to parse addr, " << context);
        return false;
    }

    if (!GetRequiredField(rankAddrJson, "plane_id", out.planeId, context)) {
        return false;
    }
    GetOptionalField(rankAddrJson, "ports", out.ports);
    return true;
}

bool TopoReader::ParseRootInfoJson(const nlohmann::json& rootInfoJson, uint32_t phyId, RootInfo& out)
{
    if (!GetRequiredField(rootInfoJson, "topo_file_path", out.topoFilePath)) {
        return false;
    }

    const auto* rankList = GetRequiredArray(rootInfoJson, "rank_list", false);
    if (rankList == nullptr) {
        return false;
    }

    for (const auto& rankJson : *rankList) {
        uint32_t deviceId = 0;
        uint32_t localId = 0;
        if (!GetRequiredField(rankJson, "device_id", deviceId) || !GetRequiredField(rankJson, "local_id", localId)) {
            return false;
        }
        if (deviceId != phyId) {
            continue;
        }

        out.deviceId = deviceId;
        out.localId = localId;
        const std::string context = "phyId " + std::to_string(phyId) + ", localId " + std::to_string(localId);

        const auto* levelList = GetRequiredArray(rankJson, "level_list", true, context);
        if (levelList == nullptr) {
            return false;
        }

        uint32_t eidIndex = 0;
        for (const auto& levelJson : *levelList) {
            auto levelInfo = std::make_shared<LevelInfo>();
            if (!ParseLevelInfo(levelJson, *levelInfo)) {
                return false;
            }
            out.levels.push_back(levelInfo);

            const auto* rankAddrList = GetRequiredArray(levelJson, "rank_addr_list", false, context);
            if (rankAddrList == nullptr) {
                return false;
            }

            for (const auto& rankAddrJson : *rankAddrList) {
                auto rankAddr = std::make_shared<RankAddr>();
                if (!ParseRankAddr(rankAddrJson, localId, eidIndex, levelInfo, *rankAddr)) {
                    SHM_LOG_WARN("Rootinfo: skip invalid rank_addr, localId " << localId << ", eidIndex " << eidIndex);
                    ++eidIndex;
                    continue;
                }
                out.eidIndexToRankAddr[eidIndex] = rankAddr;
                for (const auto& port : rankAddr->ports) {
                    out.portsToRankAddr[port] = rankAddr;
                }
                ++eidIndex;
            }
        }

        out.totalEidCount = static_cast<uint32_t>(out.eidIndexToRankAddr.size());
        if (out.totalEidCount == 0) {
            SHM_LOG_ERROR("Rootinfo: invalid eid count for phyId " << phyId << ", localId " << localId);
            return false;
        }

        SHM_LOG_INFO(
            "Parse rootinfo success for phyId " << phyId << ", localId " << localId << ", eidCount "
                                                << out.totalEidCount);
        return true;
    }

    SHM_LOG_ERROR("Rootinfo: no rank entry found for phyId " << phyId);
    return false;
}

bool TopoReader::ParseTopoInfo(const std::string& path, TopoInfo& out)
{
    out = TopoInfo{};
    std::string realPath;
    if (!CheckTopoFilePath(path, realPath)) {
        return false;
    }

    std::ifstream topoFile(realPath);
    if (!topoFile.is_open()) {
        SHM_LOG_ERROR("Failed to open topo file: " << realPath);
        return false;
    }
    nlohmann::json topoInfoJson;
    try {
        topoFile >> topoInfoJson;
    } catch (const nlohmann::json::exception& e) {
        SHM_LOG_ERROR("Topo parse failed: " << e.what());
        return false;
    }
    SHM_LOG_INFO("Read topo json from " << realPath);

    const auto* edgeList = GetRequiredArray(topoInfoJson, "edge_list", false);
    if (edgeList == nullptr) {
        return false;
    }

    for (const auto& edgeObj : *edgeList) {
        if (!edgeObj.contains("local_a")) {
            continue;
        }

        // CLOS edges describe a scale-out attachment (single endpoint, no peer local_b);
        // every other edge is a point-to-point mesh link between local_a and local_b.
        bool isClos = false;
        std::string topoType;
        if (GetOptionalField(edgeObj, "topo_type", topoType)) {
            isClos = topoType == "CLOS";
        }
        if (!edgeObj.contains("local_b")) {
            isClos = true;
        }

        if (isClos) {
            ClosTopoEdge edge;
            edge.netLayer = 0;
            edge.topoInstanceId = 0;
            if (!GetRequiredField(edgeObj, "local_a", edge.localA) ||
                !GetRequiredField(edgeObj, "net_layer", edge.netLayer) ||
                !GetRequiredField(edgeObj, "topo_instance_id", edge.topoInstanceId)) {
                return false;
            }
            GetOptionalField(edgeObj, "local_a_ports", edge.ports);
            out.closTopoEdges.push_back(std::move(edge));
            continue;
        }

        MeshTopoEdge edge;
        if (!GetRequiredField(edgeObj, "local_a", edge.localA) || !GetRequiredField(edgeObj, "local_b", edge.localB)) {
            return false;
        }
        GetOptionalField(edgeObj, "local_a_ports", edge.localAPorts);
        GetOptionalField(edgeObj, "local_b_ports", edge.localBPorts);
        out.meshTopoEdges.push_back(std::move(edge));
    }

    if (out.meshTopoEdges.empty() && out.closTopoEdges.empty()) {
        SHM_LOG_ERROR("Topo parse failed: no valid edge entries parsed.");
        return false;
    }

    // Keep CLOS edges ordered by netLayer ascending so route lookup probes the
    // closest scale-out layer first.
    std::stable_sort(
        out.closTopoEdges.begin(), out.closTopoEdges.end(),
        [](const ClosTopoEdge& lhs, const ClosTopoEdge& rhs) { return lhs.netLayer < rhs.netLayer; });
    BuildTopoIndexes(out);
    return true;
}

bool TopoReader::GetLocalId(const RootInfo& root, uint32_t deviceId, uint32_t& localId)
{
    if (!root.eidIndexToRankAddr.empty() && root.deviceId == deviceId) {
        localId = root.localId;
        return true;
    }
    SHM_LOG_ERROR("RootInfo is invalid or incomplete, failed to find localId for deviceId " << deviceId);
    return false;
}

bool TopoReader::GetEidCount(const RootInfo& root, uint32_t& count)
{
    if (root.totalEidCount > 0) {
        count = root.totalEidCount;
        return true;
    }
    SHM_LOG_ERROR("RootInfo is invalid or incomplete, failed to find a valid eid count.");
    return false;
}

bool TopoReader::ParseRdmaNetAddr(uint32_t phyId, net_addr_t& outIp)
{
    nlohmann::json rootInfoJson;
    if (!LoadRootInfoJson(phyId, rootInfoJson)) {
        SHM_LOG_ERROR("Failed to load rootinfo for phyId " << phyId);
        return false;
    }

    if (!rootInfoJson.contains("rank_list") || !rootInfoJson["rank_list"].is_array()) {
        SHM_LOG_ERROR("Rootinfo: missing or invalid rank_list, phyId " << phyId);
        return false;
    }

    bool found = false;
    const auto& rankListJson = rootInfoJson["rank_list"];
    for (const auto& rankJson : rankListJson) {
        if (!rankJson.contains("device_id") || !rankJson.contains("local_id")) {
            continue;
        }
        uint32_t rankDeviceId = 0;
        if (!ParseUint(rankJson["device_id"], rankDeviceId)) {
            continue;
        }
        if (rankDeviceId != phyId) {
            continue;
        }

        if (!rankJson.contains("level_list") || !rankJson["level_list"].is_array() || rankJson["level_list"].empty()) {
            SHM_LOG_WARN("Rootinfo: missing level_list for phyId " << phyId);
            return false;
        }

        const auto& levelListJson = rankJson["level_list"];
        for (const auto& levelJson : levelListJson) {
            if (!levelJson.contains("net_type") || !levelJson["net_type"].is_string()) {
                continue;
            }
            if (levelJson["net_type"].get<std::string>() != "CLOS") {
                continue;
            }
            if (!levelJson.contains("rank_addr_list") || !levelJson["rank_addr_list"].is_array()) {
                SHM_LOG_WARN("Rootinfo: missing rank_addr_list for phyId " << phyId);
                return false;
            }

            const auto& rankAddrListJson = levelJson["rank_addr_list"];
            for (const auto& rankAddrJson : rankAddrListJson) {
                if (!rankAddrJson.contains("addr_type")) {
                    continue;
                }
                if (!rankAddrJson["addr_type"].is_string()) {
                    SHM_LOG_ERROR("Rootinfo addr_type format is unsupported, phyId " << phyId);
                    return false;
                }
                if (!rankAddrJson.contains("addr") || !rankAddrJson["addr"].is_string()) {
                    SHM_LOG_WARN("Rootinfo: missing or invalid addr field in CLOS entry, phyId " << phyId);
                    continue;
                }
                std::string addrType = rankAddrJson["addr_type"].get<std::string>();
                std::string addrStr = rankAddrJson["addr"].get<std::string>();
                if (addrType == "IPV4") {
                    if (inet_pton(AF_INET, addrStr.c_str(), &outIp.ip.ipv4) != 1) {
                        SHM_LOG_WARN(
                            "Rootinfo: invalid IPv4 addr in CLOS entry, phyId " << phyId << ", addr " << addrStr);
                        continue;
                    }
                    outIp.type = IpV4;
                    found = true;
                } else if (addrType == "IPV6") {
                    if (inet_pton(AF_INET6, addrStr.c_str(), &outIp.ip.ipv6) != 1) {
                        SHM_LOG_WARN(
                            "Rootinfo: invalid IPv6 addr in CLOS entry, phyId " << phyId << ", addr " << addrStr);
                        continue;
                    }
                    outIp.type = IpV6;
                    found = true;
                }
            }
        }
        break;
    }

    if (!found) {
        SHM_LOG_ERROR("Rootinfo: no valid CLOS address found for phyId " << phyId);
    }
    return found;
}

bool EidConverter::Convert(RankAddrType addrType, const nlohmann::json& addr, EidData& raw)
{
    bool converted = false;
    switch (addrType) {
        case RankAddrType::EID:
            converted = FromPlainEid(addrType, addr, raw);
            break;
        case RankAddrType::IPV4:
            converted = FromIpv4(addrType, addr, raw);
            break;
        case RankAddrType::IPV6:
            converted = FromIpv6(addrType, addr, raw);
            break;
    }
    if (!converted) {
        SHM_LOG_ERROR("Rootinfo: failed to parse addr as " << AddrTypeName(addrType));
        return false;
    }
    return true;
}

bool EidConverter::FromPlainEid(RankAddrType addrType, const nlohmann::json& addr, EidData& raw)
{
    raw.fill(0);
    if (addr.is_array()) {
        if (addr.size() != raw.size()) {
            SHM_LOG_ERROR("Rootinfo " << AddrTypeName(addrType) << " addr array size is invalid: " << addr.size());
            return false;
        }
        for (size_t i = 0; i < raw.size(); ++i) {
            if (!addr[i].is_number_unsigned() && !addr[i].is_number_integer()) {
                SHM_LOG_ERROR("Rootinfo addr array contains non-numeric value at index " << i);
                return false;
            }
            auto value = addr[i].get<int32_t>();
            if (value < 0 || value > 0xff) {
                SHM_LOG_ERROR("Rootinfo addr array value out of byte range at index " << i << ", value " << value);
                return false;
            }
            raw[i] = static_cast<uint8_t>(value);
        }
        return true;
    }

    if (!addr.is_string()) {
        SHM_LOG_ERROR("Rootinfo addr format is unsupported.");
        return false;
    }

    const std::string& jsonString = addr.get_ref<const std::string&>();
    if (jsonString.empty()) {
        SHM_LOG_ERROR("Rootinfo addr hex string is empty.");
        return false;
    }

    std::string normalized;
    normalized.reserve(jsonString.size());
    for (const char ch : jsonString) {
        const auto uchar = static_cast<unsigned char>(ch);
        if (!std::isxdigit(uchar)) {
            SHM_LOG_ERROR("Rootinfo addr contains a non-hex character.");
            return false;
        }
        normalized.push_back(static_cast<char>(std::tolower(uchar)));
    }

    // Tolerate odd-length hex by left-padding to the full EID width.
    if (normalized.size() < URMA_EID_HEX_SIZE) {
        normalized.insert(normalized.begin(), URMA_EID_HEX_SIZE - normalized.size(), '0');
    }
    if (normalized.size() != URMA_EID_HEX_SIZE) {
        SHM_LOG_ERROR("Rootinfo addr hex length is invalid: " << normalized.size());
        return false;
    }

    try {
        for (size_t i = 0; i < raw.size(); ++i) {
            const std::string byteStr = normalized.substr(i * 2, 2);
            raw[i] = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        }
    } catch (const std::exception& ex) {
        SHM_LOG_ERROR("Failed to parse rootinfo addr hex string, error: " << ex.what());
        return false;
    }
    return true;
}

bool EidConverter::FromIpv4(RankAddrType addrType, const nlohmann::json& addr, EidData& raw)
{
    if (!addr.is_string()) {
        SHM_LOG_ERROR("Rootinfo " << AddrTypeName(addrType) << " addr must be a string.");
        return false;
    }
    const std::string& addrStr = addr.get_ref<const std::string&>();

    in_addr ipv4Addr{};
    if (inet_pton(AF_INET, addrStr.c_str(), &ipv4Addr) != 1) {
        SHM_LOG_ERROR("Rootinfo IPV4 addr is invalid: " << addrStr);
        return false;
    }

    raw.fill(0);
    raw[10] = static_cast<uint8_t>((URMA_EID_IPV4_PREFIX >> 8) & 0xff);
    raw[11] = static_cast<uint8_t>(URMA_EID_IPV4_PREFIX & 0xff);

    const auto* ipv4Bytes = reinterpret_cast<const uint8_t*>(&ipv4Addr.s_addr);
    std::copy_n(ipv4Bytes, sizeof(ipv4Addr), raw.begin() + 12);
    SHM_LOG_INFO("Rootinfo IPV4 addr converted to EID, addr " << addrStr);
    return true;
}

bool EidConverter::FromIpv6(RankAddrType addrType, const nlohmann::json& addr, EidData& raw)
{
    if (!addr.is_string()) {
        SHM_LOG_ERROR("Rootinfo " << AddrTypeName(addrType) << " addr must be a string.");
        return false;
    }
    const std::string& addrStr = addr.get_ref<const std::string&>();

    raw.fill(0);
    if (inet_pton(AF_INET6, addrStr.c_str(), raw.data()) != 1) {
        SHM_LOG_ERROR("Rootinfo IPV6 addr is invalid: " << addrStr);
        return false;
    }
    SHM_LOG_INFO("Rootinfo IPV6 addr converted to EID, addr " << addrStr);
    return true;
}

bool TopoReader::ParseUint(const nlohmann::json& jsonValue, uint32_t& value)
{
    if (!TryGet(jsonValue, value)) {
        SHM_LOG_ERROR("Failed to parse uint value.");
        return false;
    }
    return true;
}

namespace {

bool PortsContain(const std::vector<EidPort>& ports, const EidPort& port)
{
    return std::find(ports.begin(), ports.end(), port) != ports.end();
}

bool EndpointHasPort(const SyncEndpoint& endpoint, const EidPort& port) { return PortsContain(endpoint.ports, port); }

void BuildTopoIndexes(TopoInfo& topoInfo)
{
    topoInfo.portFabricMask.clear();
    topoInfo.meshEdgeByLocalPair.clear();
    topoInfo.closEdgeIndicesByLocalA.clear();

    for (size_t i = 0; i < topoInfo.meshTopoEdges.size(); ++i) {
        const auto& edge = topoInfo.meshTopoEdges[i];
        topoInfo.meshEdgeByLocalPair.emplace(MakeLocalPairKey(edge.localA, edge.localB), i);
        for (const auto& port : edge.localAPorts) {
            topoInfo.portFabricMask[port] |= FABRIC_MESH_MASK;
        }
        for (const auto& port : edge.localBPorts) {
            topoInfo.portFabricMask[port] |= FABRIC_MESH_MASK;
        }
    }

    for (size_t i = 0; i < topoInfo.closTopoEdges.size(); ++i) {
        const auto& edge = topoInfo.closTopoEdges[i];
        topoInfo.closEdgeIndicesByLocalA[edge.localA].push_back(i);
        for (const auto& port : edge.ports) {
            topoInfo.portFabricMask[port] |= FABRIC_CLOS_MASK;
        }
    }
}

bool PortInMeshTopo(const TopoInfo& topoInfo, const EidPort& port)
{
    if (!topoInfo.portFabricMask.empty()) {
        const auto it = topoInfo.portFabricMask.find(port);
        return it != topoInfo.portFabricMask.end() && (it->second & FABRIC_MESH_MASK) != 0;
    }
    for (const auto& edge : topoInfo.meshTopoEdges) {
        if (PortsContain(edge.localAPorts, port) || PortsContain(edge.localBPorts, port)) {
            return true;
        }
    }
    return false;
}

bool PortInClosTopo(const TopoInfo& topoInfo, const EidPort& port)
{
    if (!topoInfo.portFabricMask.empty()) {
        const auto it = topoInfo.portFabricMask.find(port);
        return it != topoInfo.portFabricMask.end() && (it->second & FABRIC_CLOS_MASK) != 0;
    }
    for (const auto& edge : topoInfo.closTopoEdges) {
        if (PortsContain(edge.ports, port)) {
            return true;
        }
    }
    return false;
}

bool IsMeshEndpoint(const TopoInfo& topoInfo, const SyncEndpoint& endpoint)
{
    if (endpoint.addrType != RankAddrType::EID) {
        return false;
    }
    if (endpoint.netType == NetType::Mesh) {
        return true;
    }
    if (endpoint.netType != NetType::TopoFileDesc || endpoint.fabric != TopoFabric::Mesh) {
        return false;
    }
    return std::any_of(
        endpoint.ports.begin(), endpoint.ports.end(), [&](const auto& port) { return PortInMeshTopo(topoInfo, port); });
}

std::string MeshNetInstanceId(
    const TopoInfo& topoInfo, const std::vector<std::vector<SyncEndpoint>>& rankIdxToSyncEndpoint, uint32_t rank)
{
    if (rank >= rankIdxToSyncEndpoint.size()) {
        return {};
    }
    for (const auto& endpoint : rankIdxToSyncEndpoint[rank]) {
        if (endpoint.netLayer == 0 && IsMeshEndpoint(topoInfo, endpoint)) {
            return endpoint.netInstanceId;
        }
    }
    return {};
}

std::string LayerZeroNetInstanceId(
    const TopoInfo& topoInfo, const std::vector<std::vector<SyncEndpoint>>& rankIdxToSyncEndpoint, uint32_t rank)
{
    if (rank >= rankIdxToSyncEndpoint.size()) {
        return {};
    }
    for (const auto& endpoint : rankIdxToSyncEndpoint[rank]) {
        if (endpoint.netLayer == 0 && !endpoint.netInstanceId.empty() && IsMeshEndpoint(topoInfo, endpoint)) {
            return endpoint.netInstanceId;
        }
    }
    return {};
}

bool IsUnknownNetInstanceId(const std::string& instanceId)
{
    static constexpr char UNKNOWN[] = "unknown";
    return instanceId.size() == sizeof(UNKNOWN) - 1 &&
           std::equal(instanceId.begin(), instanceId.end(), UNKNOWN, [](unsigned char lhs, unsigned char rhs) {
               return std::tolower(lhs) == std::tolower(rhs);
           });
}

bool IsUsableNetInstanceId(const std::string& instanceId)
{
    return !instanceId.empty() && !IsUnknownNetInstanceId(instanceId);
}

bool ResolveLocalEidByIndex(const RootInfo& root, uint32_t eidIndex, EidData& localEidRaw)
{
    const auto item = root.eidIndexToRankAddr.find(eidIndex);
    if (item == root.eidIndexToRankAddr.end() || item->second == nullptr) {
        SHM_LOG_ERROR("Failed to get local eid raw, eidIndex " << eidIndex << " not indexed.");
        return false;
    }
    localEidRaw = item->second->eidData;
    return true;
}

bool UsesMergedBaseLayer(const RootInfo& rootInfo, const TopoInfo& topoInfo)
{
    const bool hasMergedRootLevel = std::any_of(rootInfo.levels.begin(), rootInfo.levels.end(), [](const auto& level) {
        return level != nullptr && level->netLayer == 0 && level->netType == NetType::TopoFileDesc;
    });
    if (!hasMergedRootLevel || topoInfo.meshTopoEdges.empty()) {
        return false;
    }
    return std::any_of(topoInfo.closTopoEdges.begin(), topoInfo.closTopoEdges.end(), [](const auto& edge) {
        return edge.netLayer == 0;
    });
}

uint32_t EffectiveRouteNetLayer(
    const RootInfo& rootInfo, const TopoInfo& topoInfo, TopoFabric fabric, uint32_t rawNetLayer)
{
    // A merged UBX-style rootinfo puts FullMesh and CLOS addresses in one raw layer-0
    // TOPO_FILE_DESC level. Keep that raw value for topology edge/endpoint
    // matching, but expose the product-level network semantics as Mesh=0 and
    // CLOS=1 to per-QP policy selection, saved routes and the public getter.
    if (UsesMergedBaseLayer(rootInfo, topoInfo) && rawNetLayer == 0 && fabric == TopoFabric::Clos) {
        return 1;
    }
    return rawNetLayer;
}

bool EndpointMatchesFabric(const SyncEndpoint& endpoint, TopoFabric fabric)
{
    return fabric == TopoFabric::Unknown || endpoint.fabric == fabric;
}

bool PortMatchesFabric(const TopoInfo& topoInfo, const std::string& port, TopoFabric fabric)
{
    switch (fabric) {
        case TopoFabric::Mesh:
            return PortInMeshTopo(topoInfo, port);
        case TopoFabric::Clos:
            return PortInClosTopo(topoInfo, port);
        case TopoFabric::Unknown:
        default:
            return true;
    }
}

bool EndpointMatchesResolvedFabric(
    const TopoInfo& topoInfo, const SyncEndpoint& endpoint, const std::string& port, TopoFabric fabric)
{
    // The direct UDMA backend creates channels from EIDs. IP-address entries belong
    // to RoCE/RDMA discovery and must not become UDMA candidates merely because
    // their layer, fabric or port happens to match a topology edge.
    if (endpoint.addrType != RankAddrType::EID) {
        return false;
    }
    if (EndpointMatchesFabric(endpoint, fabric)) {
        return true;
    }
    // Only legacy topology-file endpoints without an explicit fabric may fall back to
    // topology membership. An endpoint explicitly marked for another fabric must not
    // be selected merely because both fabrics expose the same physical port.
    if (endpoint.netType != NetType::TopoFileDesc || endpoint.fabric != TopoFabric::Unknown) {
        return false;
    }
    return PortMatchesFabric(topoInfo, port, fabric);
}

// Physical inventory deliberately accepts IP and EID endpoints. UDMA route selection
// retains its separate EID-only predicate and bandwidth preference.
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

const SyncEndpoint* FindEndpointByPortPlaneAndFabric(
    const TopoInfo& topoInfo, const std::vector<SyncEndpoint>& endpoints, const std::string& port,
    const SyncEndpoint& localEndpoint, TopoFabric fabric, uint32_t netLayer)
{
    const SyncEndpoint* best = nullptr;
    size_t bestPortCount = 0;
    for (const auto& endpoint : endpoints) {
        if (!EndpointHasPort(endpoint, port) || !EndpointMatchesResolvedFabric(topoInfo, endpoint, port, fabric)) {
            continue;
        }
        if (fabric != TopoFabric::Mesh && endpoint.planeId != localEndpoint.planeId) {
            continue;
        }
        if (fabric == TopoFabric::Clos &&
            ((endpoint.netType == NetType::Clos && endpoint.netLayer != netLayer) ||
             ((localEndpoint.netType == NetType::Clos || endpoint.netType == NetType::Clos) &&
              (localEndpoint.netLayer != endpoint.netLayer ||
               localEndpoint.netInstanceId != endpoint.netInstanceId)))) {
            continue;
        }

        // Prefer the higher-bandwidth endpoint that matches the requested port.
        // UBX clos rootinfo may expose both a single-port address and an aggregated multi-port
        // address on the same plane; the aggregated entry is preferred when both are usable.
        const size_t portCount = endpoint.ports.size();
        if (best == nullptr || portCount > bestPortCount) {
            best = &endpoint;
            bestPortCount = portCount;
        }
    }
    return best;
}

bool ResolveRouteByPortsAndPlane(
    const TopoInfo& topoInfo, const RootInfo& rootInfo, const std::vector<SyncEndpoint>& localEndpoints,
    const std::vector<SyncEndpoint>& peerEndpoints, const std::string& localPort, const std::string& remotePort,
    TopoFabric fabric, uint32_t& localEidIndex, EidData& localEidRaw, uint32_t& remoteEidIndex, std::string& planeId,
    uint32_t netLayer = 0, std::string* netInstanceId = nullptr, uint32_t* resolvedNetLayer = nullptr,
    size_t* resolvedPortCount = nullptr)
{
    const SyncEndpoint* bestLocalEndpoint = nullptr;
    const SyncEndpoint* bestPeerEndpoint = nullptr;
    size_t bestLocalPortCount = 0;
    size_t bestPeerPortCount = 0;

    for (const auto& localEndpoint : localEndpoints) {
        if (!EndpointHasPort(localEndpoint, localPort) ||
            !EndpointMatchesResolvedFabric(topoInfo, localEndpoint, localPort, fabric)) {
            continue;
        }
        if (fabric != TopoFabric::Mesh && localEndpoint.planeId.empty()) {
            continue;
        }
        if (fabric == TopoFabric::Clos && localEndpoint.netType == NetType::Clos &&
            localEndpoint.netLayer != netLayer) {
            continue;
        }
        const auto* peerEndpoint =
            FindEndpointByPortPlaneAndFabric(topoInfo, peerEndpoints, remotePort, localEndpoint, fabric, netLayer);
        if (peerEndpoint == nullptr) {
            continue;
        }

        const size_t localPortCount = localEndpoint.ports.size();
        const size_t peerPortCount = peerEndpoint->ports.size();
        if (bestLocalEndpoint == nullptr || localPortCount > bestLocalPortCount ||
            (localPortCount == bestLocalPortCount && peerPortCount > bestPeerPortCount)) {
            bestLocalEndpoint = &localEndpoint;
            bestPeerEndpoint = peerEndpoint;
            bestLocalPortCount = localPortCount;
            bestPeerPortCount = peerPortCount;
        }
    }

    if (bestLocalEndpoint == nullptr || bestPeerEndpoint == nullptr) {
        return false;
    }
    if (!ResolveLocalEidByIndex(rootInfo, bestLocalEndpoint->eidIndex, localEidRaw)) {
        return false;
    }
    localEidIndex = bestLocalEndpoint->eidIndex;
    remoteEidIndex = bestPeerEndpoint->eidIndex;
    planeId = bestLocalEndpoint->planeId;
    if (netInstanceId != nullptr) {
        *netInstanceId = bestLocalEndpoint->netInstanceId;
    }
    if (resolvedNetLayer != nullptr) {
        *resolvedNetLayer = bestLocalEndpoint->netLayer;
    }
    if (resolvedPortCount != nullptr) {
        *resolvedPortCount = std::min(bestLocalEndpoint->ports.size(), bestPeerEndpoint->ports.size());
    }
    return true;
}

struct ClosRouteCandidate {
    uint32_t localEidIndex{};
    EidData localEidRaw{};
    uint32_t remoteEidIndex{};
    uint32_t effectiveNetLayer{};
    std::string netInstanceId{};
    uint32_t topoInstanceId{};
    std::string planeId{};
    EidPort localPort{};
    EidPort remotePort{};
    std::string orderedPortFirst{};
    std::string orderedPortSecond{};
    uint32_t orderedEidFirst{};
    uint32_t orderedEidSecond{};
    size_t portCount{};
};

EidRoute MakeClosEidRoute(const ClosRouteCandidate& candidate)
{
    EidRoute route{candidate.localEidIndex, candidate.localEidRaw, candidate.remoteEidIndex};
    route.netType = NetType::Clos;
    route.netLayer = candidate.effectiveNetLayer;
    route.netInstanceId = candidate.netInstanceId;
    route.planeId = candidate.planeId;
    route.portCount = candidate.portCount;
    return route;
}

std::vector<ClosRouteCandidate> BuildClosRouteCandidates(
    const RootInfo& rootInfo, const TopoInfo& topoInfo, const std::vector<SyncEndpoint>& myEndpoints,
    uint32_t myLocalId, uint32_t peerLocalId, uint32_t myRank, uint32_t peerRank,
    const std::vector<SyncEndpoint>& peerEndpoints)
{
    std::vector<ClosRouteCandidate> candidates;
    const auto processEdgePair = [&](const ClosTopoEdge& myEdge, const ClosTopoEdge& peerEdge) {
        if (peerEdge.netLayer != myEdge.netLayer || peerEdge.topoInstanceId != myEdge.topoInstanceId ||
            peerEdge.ports.empty()) {
            return;
        }
        for (const auto& localPort : myEdge.ports) {
            for (const auto& remotePort : peerEdge.ports) {
                uint32_t localEidIndex = 0;
                EidData localEidRaw{};
                uint32_t remoteEidIndex = 0;
                std::string planeId;
                std::string netInstanceId;
                size_t resolvedPortCount = 0;
                if (!ResolveRouteByPortsAndPlane(
                        topoInfo, rootInfo, myEndpoints, peerEndpoints, localPort, remotePort, TopoFabric::Clos,
                        localEidIndex, localEidRaw, remoteEidIndex, planeId, myEdge.netLayer, &netInstanceId, nullptr,
                        &resolvedPortCount)) {
                    continue;
                }
                ClosRouteCandidate candidate;
                candidate.localEidIndex = localEidIndex;
                candidate.localEidRaw = localEidRaw;
                candidate.remoteEidIndex = remoteEidIndex;
                candidate.effectiveNetLayer =
                    EffectiveRouteNetLayer(rootInfo, topoInfo, TopoFabric::Clos, myEdge.netLayer);
                candidate.netInstanceId = std::move(netInstanceId);
                candidate.topoInstanceId = myEdge.topoInstanceId;
                candidate.planeId = std::move(planeId);
                candidate.localPort = localPort;
                candidate.remotePort = remotePort;
                if (myRank < peerRank) {
                    candidate.orderedPortFirst = localPort;
                    candidate.orderedPortSecond = remotePort;
                    candidate.orderedEidFirst = localEidIndex;
                    candidate.orderedEidSecond = remoteEidIndex;
                } else {
                    candidate.orderedPortFirst = remotePort;
                    candidate.orderedPortSecond = localPort;
                    candidate.orderedEidFirst = remoteEidIndex;
                    candidate.orderedEidSecond = localEidIndex;
                }
                candidate.portCount = resolvedPortCount;
                candidates.push_back(std::move(candidate));
            }
        }
    };

    if (!topoInfo.closEdgeIndicesByLocalA.empty()) {
        const auto myIt = topoInfo.closEdgeIndicesByLocalA.find(myLocalId);
        const auto peerIt = topoInfo.closEdgeIndicesByLocalA.find(peerLocalId);
        if (myIt == topoInfo.closEdgeIndicesByLocalA.end() || peerIt == topoInfo.closEdgeIndicesByLocalA.end()) {
            return candidates;
        }
        for (const auto myEdgeIndex : myIt->second) {
            const auto& myEdge = topoInfo.closTopoEdges[myEdgeIndex];
            if (myEdge.ports.empty()) {
                continue;
            }
            for (const auto peerEdgeIndex : peerIt->second) {
                const auto& peerEdge = topoInfo.closTopoEdges[peerEdgeIndex];
                processEdgePair(myEdge, peerEdge);
            }
        }
    } else {
        for (const auto& myEdge : topoInfo.closTopoEdges) {
            if (myEdge.ports.empty() || myEdge.localA != myLocalId) {
                continue;
            }
            for (const auto& peerEdge : topoInfo.closTopoEdges) {
                if (peerEdge.localA != peerLocalId) {
                    continue;
                }
                processEdgePair(myEdge, peerEdge);
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.effectiveNetLayer != rhs.effectiveNetLayer) {
            return lhs.effectiveNetLayer < rhs.effectiveNetLayer;
        }
        if (lhs.portCount != rhs.portCount) {
            return lhs.portCount > rhs.portCount;
        }
        if (lhs.topoInstanceId != rhs.topoInstanceId) {
            return lhs.topoInstanceId < rhs.topoInstanceId;
        }
        if (lhs.planeId != rhs.planeId) {
            return lhs.planeId < rhs.planeId;
        }
        if (lhs.orderedPortFirst != rhs.orderedPortFirst) {
            return lhs.orderedPortFirst < rhs.orderedPortFirst;
        }
        if (lhs.orderedPortSecond != rhs.orderedPortSecond) {
            return lhs.orderedPortSecond < rhs.orderedPortSecond;
        }
        if (lhs.orderedEidFirst != rhs.orderedEidFirst) {
            return lhs.orderedEidFirst < rhs.orderedEidFirst;
        }
        if (lhs.orderedEidSecond != rhs.orderedEidSecond) {
            return lhs.orderedEidSecond < rhs.orderedEidSecond;
        }
        return false;
    });
    // Sort before deduplication so both ranks retain the same highest-priority port pair.
    // Aggregated addresses may resolve several port pairs to the same EID route.
    // Layer and topology metadata are not HCOMM path selectors; candidates with the same
    // local/remote EIDs and plane therefore represent one usable route.
    std::set<std::tuple<uint32_t, uint32_t, std::string>> seenRoutes;
    candidates.erase(
        std::remove_if(
            candidates.begin(), candidates.end(),
            [&](const auto& candidate) {
                return !seenRoutes.emplace(candidate.localEidIndex, candidate.remoteEidIndex, candidate.planeId).second;
            }),
        candidates.end());
    return candidates;
}

} // namespace

bool TopoQuerier::GetEidRouteMesh1D(uint32_t targetRank, EidRoute& route)
{
    if (myRank_ >= rankIdxToSyncEndpoint_.size() || targetRank >= rankIdxToSyncEndpoint_.size()) {
        SHM_LOG_ERROR("Mesh1D route failed, rank out of range, myRank " << myRank_ << ", targetRank " << targetRank);
        return false;
    }

    // Driver Mesh edges use server-local IDs, so TOPO_FILE_DESC must retain
    // the layer-0 instance boundary before those IDs are matched.
    const std::string myInstance = MeshNetInstanceId(topoInfo_, rankIdxToSyncEndpoint_, myRank_);
    const std::string targetInstance = MeshNetInstanceId(topoInfo_, rankIdxToSyncEndpoint_, targetRank);
    const bool hasExplicitMeshLevel = !myInstance.empty() || !targetInstance.empty();
    if (hasExplicitMeshLevel && (myInstance.empty() || myInstance != targetInstance)) {
        SHM_LOG_DEBUG(
            "Mesh1D route skipped, mesh net_instance mismatch for rank "
            << myRank_ << " (\"" << myInstance << "\") and target rank " << targetRank << " (\"" << targetInstance
            << "\").");
        return false;
    }

    if (myRank_ >= rankToLocalId_.size() || targetRank >= rankToLocalId_.size()) {
        SHM_LOG_ERROR("Mesh1D route failed, rank out of range, myRank " << myRank_ << ", targetRank " << targetRank);
        return false;
    }
    const uint32_t myLocalId = rankToLocalId_[myRank_];
    const uint32_t peerLocalId = rankToLocalId_[targetRank];
    const auto& myEndpoints = rankIdxToSyncEndpoint_[myRank_];
    const auto& peerEndpoints = rankIdxToSyncEndpoint_[targetRank];

    std::string localPort;
    std::string remotePort;
    if (!topoInfo_.meshEdgeByLocalPair.empty()) {
        const auto meshIt = topoInfo_.meshEdgeByLocalPair.find(MakeLocalPairKey(myLocalId, peerLocalId));
        if (meshIt != topoInfo_.meshEdgeByLocalPair.end()) {
            const auto& edge = topoInfo_.meshTopoEdges[meshIt->second];
            if (edge.localA == myLocalId && edge.localB == peerLocalId && !edge.localAPorts.empty() &&
                !edge.localBPorts.empty()) {
                localPort = edge.localAPorts[0];
                remotePort = edge.localBPorts[0];
            } else if (
                edge.localB == myLocalId && edge.localA == peerLocalId && !edge.localBPorts.empty() &&
                !edge.localAPorts.empty()) {
                localPort = edge.localBPorts[0];
                remotePort = edge.localAPorts[0];
            }
        }
    } else {
        for (const auto& edge : topoInfo_.meshTopoEdges) {
            if (edge.localA == myLocalId && edge.localB == peerLocalId && !edge.localAPorts.empty() &&
                !edge.localBPorts.empty()) {
                localPort = edge.localAPorts[0];
                remotePort = edge.localBPorts[0];
                break;
            }
            if (edge.localB == myLocalId && edge.localA == peerLocalId && !edge.localBPorts.empty() &&
                !edge.localAPorts.empty()) {
                localPort = edge.localBPorts[0];
                remotePort = edge.localAPorts[0];
                break;
            }
        }
    }
    if (localPort.empty() || remotePort.empty()) {
        SHM_LOG_DEBUG(
            "Mesh1D route not found, no direct mesh link between localId " << myLocalId << " and " << peerLocalId);
        return false;
    }

    uint32_t localEidIndex = 0;
    uint32_t remoteEidIndex = 0;
    uint32_t netLayer = 0;
    EidData localEidRaw{};
    std::string planeId;
    std::string netInstanceId;
    if (!ResolveRouteByPortsAndPlane(
            topoInfo_, rootInfo_, myEndpoints, peerEndpoints, localPort, remotePort, TopoFabric::Mesh, localEidIndex,
            localEidRaw, remoteEidIndex, planeId, 0, &netInstanceId, &netLayer)) {
        SHM_LOG_ERROR(
            "Mesh1D route failed, no shared plane found for local port " << localPort << " and peer port " << remotePort
                                                                         << ".");
        return false;
    }
    route = EidRoute{localEidIndex, localEidRaw, remoteEidIndex};
    route.netType = NetType::Mesh;
    route.netLayer = EffectiveRouteNetLayer(rootInfo_, topoInfo_, TopoFabric::Mesh, netLayer);
    route.netInstanceId = std::move(netInstanceId);
    route.planeId = planeId;
    // Mesh routing selects one direct port pair, not the whole endpoint's port set.
    route.portCount = 1;
    SHM_LOG_INFO(
        "Mesh1D route success, myRank " << myRank_ << ", targetRank " << targetRank << ", localPort " << localPort
                                        << ", remotePort " << remotePort << ", planeId " << planeId
                                        << ", localEidIndex " << localEidIndex << ", remoteEidIndex "
                                        << remoteEidIndex);
    return true;
}

Result TopoQuerier::ResolveEidRoutes(uint32_t targetRank, uint32_t routeCount, std::vector<EidRoute>& routes)
{
    routes.clear();
    if (routeCount == 0) {
        SHM_LOG_ERROR("EID route count must be positive for target rank " << targetRank);
        return ACLSHMEM_INVALID_VALUE;
    }

    EidRoute meshRoute;
    const bool hasMesh = GetEidRouteMesh1D(targetRank, meshRoute);

    if (myRank_ >= rankIdxToSyncEndpoint_.size() || targetRank >= rankIdxToSyncEndpoint_.size()) {
        SHM_LOG_ERROR("Clos route failed, rank out of range, myRank " << myRank_ << ", targetRank " << targetRank);
        return ACLSHMEM_INVALID_VALUE;
    }
    if (myRank_ >= rankToLocalId_.size() || targetRank >= rankToLocalId_.size()) {
        SHM_LOG_ERROR("Clos route failed, local id out of range, myRank " << myRank_ << ", targetRank " << targetRank);
        return ACLSHMEM_INVALID_VALUE;
    }
    const auto candidates = BuildClosRouteCandidates(
        rootInfo_, topoInfo_, rankIdxToSyncEndpoint_[myRank_], rankToLocalId_[myRank_], rankToLocalId_[targetRank],
        myRank_, targetRank, rankIdxToSyncEndpoint_[targetRank]);
    std::vector<EidRoute> allChoices;
    allChoices.reserve(candidates.size() + static_cast<size_t>(hasMesh));
    if (hasMesh) {
        allChoices.push_back(std::move(meshRoute));
    }
    for (const auto& candidate : candidates) {
        allChoices.push_back(MakeClosEidRoute(candidate));
    }
    // BuildClosRouteCandidates already returns Clos routes in deterministic order.
    // There is at most one Mesh route, and it is intentionally inserted first
    // (effective layer 0) before the Clos candidates (effective layer 1).

    const std::string myLayerZeroInstance = LayerZeroNetInstanceId(topoInfo_, rankIdxToSyncEndpoint_, myRank_);
    const std::string peerLayerZeroInstance = LayerZeroNetInstanceId(topoInfo_, rankIdxToSyncEndpoint_, targetRank);
    const bool hasUsableLayerZeroInstances =
        IsUsableNetInstanceId(myLayerZeroInstance) && IsUsableNetInstanceId(peerLayerZeroInstance);
    // "unknown" is a placeholder, not a shared domain identifier. If both endpoints carry that placeholder,
    // apply same-domain policy only when this peer pair has an actual direct Mesh route; otherwise use CLOS-only
    // cross-domain selection. Empty instance IDs retain the legacy cross-domain behavior.
    const bool sameLayerZeroInstance =
        hasUsableLayerZeroInstances ?
            myLayerZeroInstance == peerLayerZeroInstance :
            IsUnknownNetInstanceId(myLayerZeroInstance) && IsUnknownNetInstanceId(peerLayerZeroInstance) && hasMesh;

    std::vector<EidRoute> routeChoices;
    if (!multiLinkEnabled_ && hasMesh) {
        const auto meshIt = std::find_if(
            allChoices.begin(), allChoices.end(), [](const EidRoute& route) { return route.netType == NetType::Mesh; });
        routeChoices.push_back(*meshIt);
    } else if (!multiLinkEnabled_ || sameLayerZeroInstance) {
        routeChoices = allChoices;
    } else {
        // Mesh routes are valid only within the same layer-0 network instance.
        // Cross-domain peers ignore explicit layer policies and use Clos candidates only.
        std::copy_if(allChoices.begin(), allChoices.end(), std::back_inserter(routeChoices), [](const auto& route) {
            return route.netType == NetType::Clos;
        });
    }

    if (routeChoices.empty()) {
        SHM_LOG_ERROR(
            "EID route failed, no usable route choices for rank "
            << myRank_ << " to target rank " << targetRank << ", hasMesh=" << hasMesh
            << ", closCandidateCount=" << candidates.size() << ", configuredPolicyCount=" << netLayerPolicy_.size()
            << ", multiLink=" << multiLinkEnabled_);
        return ACLSHMEM_INNER_ERROR;
    }

    routes.reserve(routeCount);
    for (uint32_t routeIdx = 0; routeIdx < routeCount; ++routeIdx) {
        routes.push_back(routeChoices[routeIdx % routeChoices.size()]);
    }

    if (multiLinkEnabled_ && sameLayerZeroInstance && !netLayerPolicy_.empty()) {
        std::map<uint32_t, std::vector<const EidRoute*>> candidatesByLayer;
        for (const auto& candidate : allChoices) {
            candidatesByLayer[candidate.netLayer].push_back(&candidate);
        }
        std::map<uint32_t, size_t> nextByLayer;
        for (const auto& policy : netLayerPolicy_) {
            const uint32_t qpIdx = policy.first;
            const uint32_t requestedLayer = policy.second;
            if (qpIdx >= routeCount) {
                SHM_LOG_ERROR("UDMA netLayer policy QP index " << qpIdx << " exceeds route count " << routeCount);
                routes.clear();
                return ACLSHMEM_INVALID_VALUE;
            }
            const auto layerIt = candidatesByLayer.find(requestedLayer);
            if (layerIt == candidatesByLayer.end() || layerIt->second.empty()) {
                SHM_LOG_ERROR(
                    "UDMA netLayer " << requestedLayer << " is unavailable between same-domain rank " << myRank_
                                     << " and target rank " << targetRank);
                routes.clear();
                return ACLSHMEM_INVALID_VALUE;
            }
            const auto& layerCandidates = layerIt->second;
            routes[qpIdx] = *layerCandidates[nextByLayer[requestedLayer]++ % layerCandidates.size()];
            if (routes[qpIdx].netLayer != requestedLayer) {
                SHM_LOG_ERROR(
                    "Resolved UDMA netLayer " << routes[qpIdx].netLayer << " does not match requested layer "
                                              << requestedLayer << " for QP " << qpIdx);
                routes.clear();
                return ACLSHMEM_INNER_ERROR;
            }
        }
    }

    for (uint32_t routeIdx = 0; routeIdx < routes.size(); ++routeIdx) {
        const auto& route = routes[routeIdx];
        const auto policyIt = netLayerPolicy_.find(routeIdx);
        const bool explicitPolicyApplied =
            multiLinkEnabled_ && sameLayerZeroInstance && policyIt != netLayerPolicy_.end();
        SHM_LOG_INFO(
            "UDMA route selected, myRank "
            << myRank_ << ", targetRank " << targetRank << ", routeIdx " << routeIdx << ", netType "
            << static_cast<uint32_t>(route.netType) << ", netLayer " << route.netLayer << ", netInstanceId \""
            << route.netInstanceId << "\", planeId \"" << route.planeId << "\", portCount " << route.portCount
            << ", localEidIndex " << route.localEidIndex << ", remoteEidIndex " << route.remoteEidIndex
            << ", multiLink=" << multiLinkEnabled_ << ", sameLayerZeroInstance=" << sameLayerZeroInstance
            << ", source=" << (explicitPolicyApplied ? "explicit" : "default") << ", requestedNetLayer="
            << (policyIt == netLayerPolicy_.end() ? std::string("default") : std::to_string(policyIt->second))
            << ", configuredPolicyIgnored=" << (policyIt != netLayerPolicy_.end() && !explicitPolicyApplied));
    }
    return ACLSHMEM_SUCCESS;
}

Result TopoQuerier::GetEidRoutes(uint32_t targetRank, uint32_t routeCount, std::vector<EidRoute>& routes)
{
    return ResolveEidRoutes(targetRank, routeCount, routes);
}

bool TopoQuerier::GetEidRoute(
    uint32_t targetRank, uint32_t& localEidIndex, EidData& localEidRaw, uint32_t& remoteEidIndex)
{
    std::vector<EidRoute> routes;
    // This legacy single-route interface returns the default route and must not inherit
    // per-QP layer policies configured for GetEidRoutes().
    TopoQuerier defaultQuerier(
        rootInfo_, topoInfo_, myRank_, rankToLocalId_, rankIdxToSyncEndpoint_, {}, multiLinkEnabled_);
    if (defaultQuerier.ResolveEidRoutes(targetRank, 1U, routes) != ACLSHMEM_SUCCESS || routes.empty()) {
        return false;
    }
    localEidIndex = routes.front().localEidIndex;
    localEidRaw = routes.front().localEidRaw;
    remoteEidIndex = routes.front().remoteEidIndex;
    return true;
}

void TopoQuerier::GetReachableNetLayers(uint32_t targetRank, std::vector<uint32_t>& layers) const
{
    layers.clear();
    if (targetRank == myRank_ || myRank_ >= rankToLocalId_.size() || targetRank >= rankToLocalId_.size() ||
        myRank_ >= rankIdxToSyncEndpoint_.size() || targetRank >= rankIdxToSyncEndpoint_.size()) {
        return;
    }
    const auto localId = rankToLocalId_[myRank_];
    const auto peerId = rankToLocalId_[targetRank];
    const auto& local = rankIdxToSyncEndpoint_[myRank_];
    const auto& peer = rankIdxToSyncEndpoint_[targetRank];
    auto hasPair = [&](const std::vector<EidPort>& localPorts, const std::vector<EidPort>& peerPorts,
                       TopoFabric fabric, uint32_t layer) {
        for (const auto& lp : localPorts) {
            for (const auto& pp : peerPorts) {
                for (const auto& a : local) {
                    for (const auto& b : peer) {
                        if (PhysicalEndpointPairMatches(topoInfo_, a, b, lp, pp, fabric, layer)) return true;
                    }
                }
            }
        }
        return false;
    };
    auto append = [&](TopoFabric fabric, uint32_t rawLayer) {
        const auto layer = EffectiveRouteNetLayer(rootInfo_, topoInfo_, fabric, rawLayer);
        if (layer == 0 || layer == 1) layers.push_back(layer);
    };
    // The existing local-pair index stores only one edge; inventory needs every edge.
    for (const auto& edge : topoInfo_.meshTopoEdges) {
        if (edge.localA == localId && edge.localB == peerId &&
            hasPair(edge.localAPorts, edge.localBPorts, TopoFabric::Mesh, 0)) {
            append(TopoFabric::Mesh, 0);
        } else if (edge.localB == localId && edge.localA == peerId &&
                   hasPair(edge.localBPorts, edge.localAPorts, TopoFabric::Mesh, 0)) {
            append(TopoFabric::Mesh, 0);
        }
    }
    for (const auto& a : topoInfo_.closTopoEdges) {
        if (a.localA != localId) continue;
        for (const auto& b : topoInfo_.closTopoEdges) {
            if (b.localA == peerId && a.netLayer == b.netLayer && a.topoInstanceId == b.topoInstanceId &&
                hasPair(a.ports, b.ports, TopoFabric::Clos, a.netLayer)) {
                append(TopoFabric::Clos, a.netLayer);
            }
        }
    }
    if (HasRoceLayerRelation(local, peer)) layers.push_back(3);
    std::sort(layers.begin(), layers.end());
    layers.erase(std::unique(layers.begin(), layers.end()), layers.end());
}

} // namespace transport
} // namespace shm
