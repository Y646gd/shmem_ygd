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
