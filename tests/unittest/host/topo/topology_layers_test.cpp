/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#include <gtest/gtest.h>
#include "transport/topo/topo_reader.h"
#include "utils/shmemi_host_types.h"

namespace shm::transport {
namespace {
class TopologyLayersTest : public testing::Test {
protected:
    RootInfo root;
    TopoInfo topo;
    std::vector<uint32_t> ids{0, 1};
    std::vector<std::vector<SyncEndpoint>> endpoints{2};
    void Add(NetType type, uint32_t layer, const std::string& plane = "left")
    {
        const auto prefix = std::to_string(layer) + plane;
        endpoints[0].push_back({layer, "domain", type, TopoFabric::Unknown, plane, {prefix + "a"}, 0});
        endpoints[1].push_back({layer, "domain", type, TopoFabric::Unknown, plane, {prefix + "b"}, 0});
        if (type == NetType::Mesh) {
            topo.meshTopoEdges.push_back({0, 1, {prefix + "a"}, {prefix + "b"}});
        } else {
            topo.closTopoEdges.push_back({layer, 42, 0, {prefix + "a"}});
            topo.closTopoEdges.push_back({layer, 42, 1, {prefix + "b"}});
        }
    }
    std::vector<uint32_t> Query(uint32_t peer = 1, bool multi = false,
                                const std::map<uint32_t, uint32_t>& policy = {})
    {
        TopoQuerier query(root, topo, 0, ids, endpoints, policy, multi);
        std::vector<uint32_t> layers{99};
        query.GetReachableNetLayers(peer, layers);
        return layers;
    }
};

TEST_F(TopologyLayersTest, EnumeratesAllEdgesAndBothPlanesWithoutPolicy)
{
    topo.meshTopoEdges.push_back({0, 1, {"unusable"}, {"unusable"}});
    Add(NetType::Mesh, 0);
    Add(NetType::Clos, 1);
    Add(NetType::Clos, 1, "right");
    EXPECT_EQ(Query(), (std::vector<uint32_t>{0, 1}));
    EXPECT_EQ(Query(1, true, {{0, 1}, {1, 0}, {2, 1}, {3, 0}}), Query());
    EXPECT_TRUE(Query(0).empty());
    EXPECT_TRUE(Query(99).empty());
    std::swap(ids[0], ids[1]);
    std::swap(endpoints[0], endpoints[1]);
    EXPECT_EQ(Query(), (std::vector<uint32_t>{0, 1}));
}

TEST_F(TopologyLayersTest, AllEightPhysicalLayerSetsWithDuplicatePlanesAndPorts)
{
    for (uint32_t subset = 0; subset < 8; ++subset) {
        SCOPED_TRACE(subset);
        topo = {};
        endpoints = std::vector<std::vector<SyncEndpoint>>(2);
        std::vector<uint32_t> expected;
        for (uint32_t bit = 0; bit < 3; ++bit) {
            if (!(subset & (1U << bit))) continue;
            const uint32_t layer = bit == 2 ? 3 : bit;
            expected.push_back(layer);
            Add(layer == 0 ? NetType::Mesh : NetType::Clos, layer);
            Add(layer == 0 ? NetType::Mesh : NetType::Clos, layer, "right");
        }
        for (auto& rank : endpoints) for (auto& ep : rank) {
            if (ep.netLayer == 3) ep.addrType = RankAddrType::IPV4;
            // Multiple ports/EIDs in one plane still describe a single layer.
            ep.ports.push_back("extra-port");
        }
        EXPECT_EQ(Query(), expected);
        EXPECT_EQ(Query(1, true, {{0, 0}, {1, 0}, {2, 0}, {3, 0}}), expected);
        // One-sided additional IO-DIE/plane does not invent another relation.
        if (!endpoints[0].empty()) {
            auto one_sided = endpoints[0].front();
            one_sided.planeId = "one-sided";
            endpoints[0].push_back(one_sided);
            EXPECT_EQ(Query(), expected);
        }
        EXPECT_TRUE(Query(0).empty());
    }
}

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

TEST_F(TopologyLayersTest, MergedRawLayerZeroUsesPhysicalEdgesAndAcceptsIp)
{
    Add(NetType::Mesh, 0);
    Add(NetType::Clos, 0);
    root.levels.push_back(std::make_shared<LevelInfo>(LevelInfo{0, "domain", NetType::TopoFileDesc, ""}));
    for (auto& rank : endpoints) {
        for (auto& ep : rank) {
            ep.netType = NetType::TopoFileDesc;
            ep.addrType = RankAddrType::IPV6;
        }
    }
    EXPECT_EQ(Query(), (std::vector<uint32_t>{0, 1}));
    // CLOS attachments use their topology instance, not the layer-0 Mesh domain.
    endpoints[1][0].netInstanceId = "other-server";
    endpoints[1][1].netInstanceId = "other-server";
    EXPECT_EQ(Query(), (std::vector<uint32_t>{1}));
}

TEST_F(TopologyLayersTest, RoceRequiresExplicitLayerThreeClosDomainPlaneAndAddressFamily)
{
    Add(NetType::Clos, 3);
    topo.closTopoEdges.clear(); // RoCE comes from its declared network instance, not UDMA edges.
    for (auto& rank : endpoints) rank[0].addrType = RankAddrType::IPV4;
    EXPECT_EQ(Query(), (std::vector<uint32_t>{3}));
    endpoints[1][0].addrType = RankAddrType::IPV6;
    EXPECT_TRUE(Query().empty());
    endpoints[0][0].addrType = RankAddrType::IPV6;
    EXPECT_EQ(Query(), (std::vector<uint32_t>{3}));
    endpoints[1][0].netInstanceId = "isolated";
    EXPECT_TRUE(Query().empty());
    endpoints[1][0].netInstanceId = "domain";
    endpoints[1][0].planeId.clear();
    EXPECT_TRUE(Query().empty());
    endpoints[1][0].planeId = "left";
    endpoints[1][0].netLayer = 2;
    EXPECT_TRUE(Query().empty());
    endpoints[1][0].netLayer = 3;
    endpoints[1][0].netType = NetType::TopoFileDesc;
    EXPECT_TRUE(Query().empty());
}

TEST_F(TopologyLayersTest, ExplicitClosFabricWinsOverSharedMeshPorts)
{
    Add(NetType::Mesh, 0);
    auto level = std::make_shared<LevelInfo>(LevelInfo{1, "domain", NetType::Clos, ""});
    auto addr = std::make_shared<RankAddr>();
    addr->ports = endpoints[0][0].ports;
    addr->levelInfo = level;
    root.eidIndexToRankAddr.emplace(7, addr);
    const auto result = TopologyContext::BuildLocalEndpoints(root, topo);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].fabric, TopoFabric::Clos);
    EXPECT_EQ(result[0].eidIndex, 7U);
}


// RFC 3.3/4.5: physical links require both port ownership and fabric metadata.
TEST_F(TopologyLayersTest, RejectsWrongPortFabricLayerAndUnknownMeshDomain)
{
    Add(NetType::Mesh, 0);
    const auto baseline = endpoints;
    endpoints[1][0].ports = {"not-the-edge-port"};
    EXPECT_TRUE(Query().empty());
    endpoints = baseline; endpoints[1][0].netType = NetType::Clos;
    EXPECT_TRUE(Query().empty());
    endpoints = baseline; endpoints[1][0].netLayer = 1;
    EXPECT_TRUE(Query().empty());
    for (const std::string& domain : {std::string{}, std::string{"unknown"}}) {
        endpoints = baseline;
        endpoints[0][0].netInstanceId = endpoints[1][0].netInstanceId = domain;
        EXPECT_TRUE(Query().empty());
    }
    endpoints = baseline;
    auto bad = endpoints[0][0]; bad.netInstanceId = "isolated";
    endpoints[0].insert(endpoints[0].begin(), bad);
    EXPECT_EQ(Query(), (std::vector<uint32_t>{0})); // Search beyond first mismatching endpoint.
}

TEST_F(TopologyLayersTest, ClosRejectsMissingPeerPlaneAndAcceptsOneValidLaterPair)
{
    Add(NetType::Clos, 1, "left");
    endpoints[1][0].planeId = "right";
    EXPECT_TRUE(Query().empty());
    Add(NetType::Clos, 1, "valid-plane");
    EXPECT_EQ(Query(), (std::vector<uint32_t>{1}));
    endpoints[0][1].planeId.clear();
    EXPECT_TRUE(Query().empty());
}

TEST_F(TopologyLayersTest, AggregatedAndSeparatePortDescriptionsYieldSameLayer)
{
    Add(NetType::Clos, 1);
    topo.closTopoEdges[0].ports.push_back("extra-a");
    topo.closTopoEdges[1].ports.push_back("extra-b");
    endpoints[0][0].ports.push_back("extra-a"); endpoints[1][0].ports.push_back("extra-b");
    const auto expected = Query();
    ASSERT_EQ(expected, (std::vector<uint32_t>{1}));
    for (auto& rank : endpoints) {
        auto split = rank[0]; split.ports = {rank[0].ports.back()};
        split.eidIndex = 11; rank[0].ports.pop_back(); rank.push_back(split);
    }
    EXPECT_EQ(Query(), expected);
    for (auto& rank : endpoints) for (auto& ep : rank) ep.addrType = RankAddrType::IPV6;
    EXPECT_EQ(Query(), expected); // Physical inventory has no UDMA EID-only filter.
}

TEST_F(TopologyLayersTest, LayerTwoAndDefaultNeverLeakIntoPublicInventory)
{
    Add(NetType::Clos, 2);
    Add(NetType::Clos, 0xffffffffU);
    EXPECT_TRUE(Query().empty());
}

TEST_F(TopologyLayersTest, RoceRejectsDifferentNonemptyPlanesEmptyUnknownDomainsAndEid)
{
    Add(NetType::Clos, 3);
    for (auto& rank : endpoints) rank[0].addrType = RankAddrType::IPV4;
    const auto baseline = endpoints;
    endpoints[1][0].planeId = "another-plane";
    EXPECT_TRUE(Query().empty());
    for (const std::string& domain : {std::string{}, std::string{"unknown"}}) {
        endpoints = baseline;
        endpoints[0][0].netInstanceId = endpoints[1][0].netInstanceId = domain;
        EXPECT_TRUE(Query().empty());
    }
    endpoints = baseline;
    for (auto& rank : endpoints) rank[0].addrType = RankAddrType::EID;
    EXPECT_TRUE(Query().empty());
    endpoints = baseline;
    auto invalid = endpoints[0][0]; invalid.planeId = "isolated";
    endpoints[0].insert(endpoints[0].begin(), invalid);
    EXPECT_EQ(Query(), (std::vector<uint32_t>{3}));
}

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

TEST(TopologyAgreementTest, PropagatesRemoteFailureAndBootstrapErrors)
{
    aclshmemi_bootstrap_handle_t boot{};
    boot.npes = 2;
    boot.allgather = [](const void* in, void* out, int, aclshmemi_bootstrap_handle_t*) {
        static_cast<int*>(out)[0] = *static_cast<const int*>(in);
        static_cast<int*>(out)[1] = ACLSHMEM_MALLOC_FAILED;
        return 0;
    };
    EXPECT_EQ(TopologyContext::Agree(0, 2, &boot), ACLSHMEM_MALLOC_FAILED);
    boot.allgather = [](const void*, void*, int, aclshmemi_bootstrap_handle_t*) { return -1; };
    EXPECT_EQ(TopologyContext::Agree(0, 2, &boot), ACLSHMEM_BOOTSTRAP_ERROR);
    EXPECT_EQ(TopologyContext::Agree(0, 2, nullptr), ACLSHMEM_NOT_INITED);
}
} // namespace
} // namespace shm::transport
