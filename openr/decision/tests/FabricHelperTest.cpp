/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/Constants.h>
#include <openr/common/LsdbUtil.h>
#include <openr/config/Config.h>
#include <openr/decision/FabricHelper.h>
#include <openr/decision/LinkState.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>

using namespace testing;
using namespace openr;

namespace openr {

class FabricHelperTestFixture : public ::testing::Test {
 protected:
  static FabricConfig
  makeFabricConfig(
      const std::string& fabricName = "bbf01.dfw1",
      const std::vector<std::string>& leafRegexes = {"bbf01-ld\\d{3}\\.dfw1"},
      const std::vector<std::string>& spineRegexes = {"bbf01-sp\\d{3}\\.dfw1"},
      const std::vector<std::string>& controlRegexes = {
          "bbf01-lc\\d{3}\\.dfw1"}) {
    thrift::FabricConfig thriftCfg;
    thriftCfg.fabric_name() = fabricName;
    thriftCfg.fabric_leaf_regexes() = leafRegexes;
    thriftCfg.fabric_spine_regexes() = spineRegexes;
    thriftCfg.fabric_control_regexes() = controlRegexes;
    return FabricConfig(thriftCfg);
  }

  FabricHelper
  makeHelper() {
    return FabricHelper(fabricCfg_, linkMap_);
  }

  using NodeInterface = FabricHelper::NodeInterface;
  using NodeInterfaceHasher = FabricHelper::NodeInterfaceHasher;

  static folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
  getExternalNodeToLeaf(const FabricHelper& h) {
    return h.externalNodeToLeaf_;
  }

  static folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
  getLeafToExternalNode(const FabricHelper& h) {
    return h.leafToExternalNode_;
  }

  FabricConfig fabricCfg_{makeFabricConfig()};
  folly::F14NodeMap<std::string, Link::LinkSet> linkMap_;
};

namespace {

//
// FabricHelper unit tests
//

TEST_F(FabricHelperTestFixture, GetFabricName) {
  FabricHelper helper = makeHelper();
  EXPECT_THAT(helper.getFabricName(), Eq("bbf01.dfw1"));
}

TEST_F(FabricHelperTestFixture, GetRealOtherNodeName_NotFabric) {
  FabricHelper helper = makeHelper();

  // Adjacency points to a regular node, not the fabric name
  thrift::Adjacency adj = createAdjacency(
      "eb01.rva1", "po1000", "po1001", "fe80::1", "10.0.0.1", 1, 0);

  EXPECT_THAT(helper.getRealOtherNodeName("eb01.rva1", adj), Eq("eb01.rva1"));
}

TEST_F(FabricHelperTestFixture, GetRealOtherNodeName_FabricWithMapping) {
  FabricHelper helper = makeHelper();

  // Simulate bbf01-ld001.dfw1 reporting adjacency to eb01.rva1.
  // bbf01-ld001.dfw1's adj: otherNodeName="eb01.rva1", ifName="po1000",
  // otherIfName="po1001"
  thrift::Adjacency leafAdj = createAdjacency(
      "eb01.rva1", "po1000", "po1001", "fe80::2", "10.0.0.2", 1, 0);
  thrift::AdjacencyDatabase leafAdjDb =
      createAdjDb("bbf01-ld001.dfw1", {leafAdj}, 1);

  // Populate the external-to-leaf mapping
  helper.updateExternalNodeToLeafMap(leafAdjDb);

  // eb01.rva1's adj to the fabric: otherNodeName="bbf01.dfw1", ifName="po1001"
  thrift::Adjacency extAdj = createAdjacency(
      "bbf01.dfw1", "po1001", "po1000", "fe80::3", "10.0.0.3", 1, 0);

  // Should resolve "bbf01.dfw1" to "bbf01-ld001.dfw1"
  EXPECT_THAT(
      helper.getRealOtherNodeName("eb01.rva1", extAdj), Eq("bbf01-ld001.dfw1"));
}

TEST_F(FabricHelperTestFixture, GetRealOtherNodeName_FabricWithoutMapping) {
  FabricHelper helper = makeHelper();

  // eb01.rva1's adj to the fabric, but no mapping has been populated
  thrift::Adjacency extAdj = createAdjacency(
      "bbf01.dfw1", "po1001", "po1000", "fe80::3", "10.0.0.3", 1, 0);

  // Falls back to returning the fabric name
  EXPECT_THAT(
      helper.getRealOtherNodeName("eb01.rva1", extAdj), Eq("bbf01.dfw1"));
}

TEST_F(FabricHelperTestFixture, UpdateExternalNodeToLeafMap_LeafNode) {
  FabricHelper helper = makeHelper();

  // bbf01-ld001.dfw1 has adjacencies to two external nodes
  thrift::Adjacency adjToExt1 = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  thrift::Adjacency adjToExt2 = createAdjacency(
      "eb01.ftw1", "po1000", "po1002", "fe80::11", "10.0.0.11", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjToExt1, adjToExt2}, 1));

  // Verify externalNodeToLeaf_
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(2));
  NodeInterface ext1Key{"eb01.rva1", "po1000"};
  NodeInterface ext1Val{"bbf01-ld001.dfw1", "po1001"};
  EXPECT_THAT(extToLeaf[ext1Key], Eq(ext1Val));
  NodeInterface ext2Key{"eb01.ftw1", "po1002"};
  NodeInterface ext2Val{"bbf01-ld001.dfw1", "po1000"};
  EXPECT_THAT(extToLeaf[ext2Key], Eq(ext2Val));

  // Verify leafToExternalNode_
  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExt = getLeafToExternalNode(helper);
  EXPECT_THAT(leafToExt, SizeIs(1));
  EXPECT_THAT(leafToExt.at("bbf01-ld001.dfw1"), Eq(extToLeaf));
}

TEST_F(FabricHelperTestFixture, UpdateExternalNodeToLeafMap_NonLeafNode) {
  FabricHelper helper = makeHelper();

  // "eb01.rva1" does NOT match leaf regex, so mapping should not update
  thrift::Adjacency adj = createAdjacency(
      "eb01.sjc1", "po1000", "po1001", "fe80::1", "10.0.0.1", 1, 0);
  helper.updateExternalNodeToLeafMap(createAdjDb("eb01.rva1", {adj}, 1));

  EXPECT_THAT(getExternalNodeToLeaf(helper), IsEmpty());
  EXPECT_THAT(getLeafToExternalNode(helper), IsEmpty());
}

TEST_F(FabricHelperTestFixture, UpdateExternalNodeToLeafMap_StaleLinkRemoved) {
  FabricHelper helper = makeHelper();

  // Initial: leaf has two external adjacencies
  thrift::Adjacency adjToExt1 = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  thrift::Adjacency adjToExt2 = createAdjacency(
      "eb01.ftw1", "po1002", "po1003", "fe80::11", "10.0.0.11", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjToExt1, adjToExt2}, 1));

  EXPECT_THAT(getExternalNodeToLeaf(helper), SizeIs(2));

  // Update: leaf now only has ext1, ext2 is gone
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjToExt1}, 2));

  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(1));
  NodeInterface ext1Key{"eb01.rva1", "po1000"};
  NodeInterface ext1Val{"bbf01-ld001.dfw1", "po1001"};
  EXPECT_THAT(extToLeaf[ext1Key], Eq(ext1Val));
  NodeInterface ext2Key{"eb01.ftw1", "po1003"};
  EXPECT_THAT(extToLeaf.count(ext2Key), Eq(0));

  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExt = getLeafToExternalNode(helper);
  EXPECT_THAT(leafToExt, SizeIs(1));
  EXPECT_THAT(leafToExt.at("bbf01-ld001.dfw1"), Eq(extToLeaf));
}

TEST_F(
    FabricHelperTestFixture, UpdateExternalNodeToLeafMap_LeafInterfaceChanged) {
  FabricHelper helper = makeHelper();

  // Initial: leaf connects to ext via po1001
  thrift::Adjacency adjOld = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjOld}, 1));

  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(1));
  NodeInterface extKey{"eb01.rva1", "po1000"};
  NodeInterface oldLeafVal{"bbf01-ld001.dfw1", "po1001"};
  EXPECT_THAT(extToLeaf[extKey], Eq(oldLeafVal));

  // Update: same external node, but leaf interface changed to po2001
  thrift::Adjacency adjNew = createAdjacency(
      "eb01.rva1", "po2001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjNew}, 2));

  // Leaf interface updated in both maps
  extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(1));
  NodeInterface newLeafVal{"bbf01-ld001.dfw1", "po2001"};
  EXPECT_THAT(extToLeaf[extKey], Eq(newLeafVal));

  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExt = getLeafToExternalNode(helper);
  EXPECT_THAT(leafToExt, SizeIs(1));
  EXPECT_THAT(leafToExt.at("bbf01-ld001.dfw1"), Eq(extToLeaf));
}

TEST_F(FabricHelperTestFixture, UpdateExternalNodeToLeafMap_AllLinksRemoved) {
  FabricHelper helper = makeHelper();

  // Initial: leaf has one external adjacency
  thrift::Adjacency adj = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  helper.updateExternalNodeToLeafMap(createAdjDb("bbf01-ld001.dfw1", {adj}, 1));

  EXPECT_THAT(getExternalNodeToLeaf(helper), SizeIs(1));

  // Update: empty adjacency list
  helper.updateExternalNodeToLeafMap(createAdjDb("bbf01-ld001.dfw1", {}, 2));

  EXPECT_THAT(getExternalNodeToLeaf(helper), IsEmpty());
  EXPECT_THAT(getLeafToExternalNode(helper), IsEmpty());
}

TEST_F(
    FabricHelperTestFixture,
    UpdateExternalNodeToLeafMap_FabricAdjacenciesSkipped) {
  FabricHelper helper = makeHelper();

  // Leaf has adjacencies to both a spine (fabric) and an external node
  thrift::Adjacency adjToSpine = createAdjacency(
      "bbf01-sp001.dfw1", "po10100", "po10200", "fe80::1", "10.0.0.1", 1, 0);
  thrift::Adjacency adjToExt = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {adjToSpine, adjToExt}, 1));

  // Only external mapping should exist, spine is skipped
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(1));
  NodeInterface extKey{"eb01.rva1", "po1000"};
  NodeInterface leafVal{"bbf01-ld001.dfw1", "po1001"};
  EXPECT_THAT(extToLeaf[extKey], Eq(leafVal));
  NodeInterface spineKey{"bbf01-sp001.dfw1", "po10200"};
  EXPECT_THAT(extToLeaf.count(spineKey), Eq(0));
}

TEST_F(FabricHelperTestFixture, UpdateExternalNodeToLeafMap_MultipleLeaves) {
  FabricHelper helper = makeHelper();

  // Leaf 1 connects to ext1
  thrift::Adjacency leaf1Adj = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::10", "10.0.0.10", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld001.dfw1", {leaf1Adj}, 1));

  // Leaf 2 connects to ext2
  thrift::Adjacency leaf2Adj = createAdjacency(
      "eb01.ftw1", "po2001", "po2000", "fe80::11", "10.0.0.11", 1, 0);
  helper.updateExternalNodeToLeafMap(
      createAdjDb("bbf01-ld002.dfw1", {leaf2Adj}, 2));

  // Both mappings should exist independently
  folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>
      extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(2));
  NodeInterface ext1Key{"eb01.rva1", "po1000"};
  NodeInterface leaf1Val{"bbf01-ld001.dfw1", "po1001"};
  EXPECT_THAT(extToLeaf[ext1Key], Eq(leaf1Val));
  NodeInterface ext2Key{"eb01.ftw1", "po2000"};
  NodeInterface leaf2Val{"bbf01-ld002.dfw1", "po2001"};
  EXPECT_THAT(extToLeaf[ext2Key], Eq(leaf2Val));

  folly::F14NodeMap<
      std::string,
      folly::F14NodeMap<NodeInterface, NodeInterface, NodeInterfaceHasher>>
      leafToExt = getLeafToExternalNode(helper);
  EXPECT_THAT(leafToExt, SizeIs(2));

  // Removing ext from leaf1 should not affect leaf2
  helper.updateExternalNodeToLeafMap(createAdjDb("bbf01-ld001.dfw1", {}, 3));

  extToLeaf = getExternalNodeToLeaf(helper);
  EXPECT_THAT(extToLeaf, SizeIs(1));
  EXPECT_THAT(extToLeaf[ext2Key], Eq(leaf2Val));

  leafToExt = getLeafToExternalNode(helper);
  EXPECT_THAT(leafToExt, SizeIs(1));
  EXPECT_THAT(leafToExt.count("bbf01-ld001.dfw1"), Eq(0));
  EXPECT_THAT(leafToExt.at("bbf01-ld002.dfw1"), Eq(extToLeaf));
}

//
// LinkState::maybeMakeLink with FabricHelper integration tests
//

TEST_F(FabricHelperTestFixture, MaybeMakeLink_WithFabricName) {
  //
  // Topology: eb01.iad1 -- bbf01-ld001.dfw1 ("bbf01.dfw1")
  //
  // eb01.iad1 sees adjacency to "bbf01.dfw1"
  // bbf01-ld001.dfw1 sees adjacency to "eb01.iad1"
  // FabricHelper should resolve "bbf01.dfw1" to "bbf01-ld001.dfw1" so the
  // bidirectional link is detected.
  //

  // bbf01-ld001.dfw1's adjacency to eb01.iad1
  thrift::Adjacency leafToExt = createAdjacency(
      "eb01.iad1", "po1000", "po1001", "fe80::1", "10.0.0.1", 10, 100);

  // eb01.iad1's adjacency — reports "bbf01.dfw1" as the other node
  thrift::Adjacency extToFabric = createAdjacency(
      "bbf01.dfw1", "po1001", "po1000", "fe80::2", "10.0.0.2", 10, 200);

  thrift::AdjacencyDatabase leafAdjDb =
      createAdjDb("bbf01-ld001.dfw1", {leafToExt}, 1);
  thrift::AdjacencyDatabase extAdjDb =
      createAdjDb("eb01.iad1", {extToFabric}, 2);

  // Create LinkState from eb01.iad1's perspective and install FabricHelper
  LinkState state{kTestingAreaName, "eb01.iad1"};
  state.addFabricHelper(fabricCfg_);

  // First, update with leaf's adjDb — this populates the external-to-leaf
  // mapping AND stores bbf01-ld001.dfw1's adjacency database.
  LinkState::LinkStateChange update1 =
      state.updateAdjacencyDatabase(leafAdjDb, kTestingAreaName);
  EXPECT_THAT(update1.topologyChanged, IsFalse());

  // Now update with eb01.iad1's adjDb — maybeMakeLink should resolve
  // "bbf01.dfw1" to "bbf01-ld001.dfw1" and create a bidirectional link.
  LinkState::LinkStateChange update2 =
      state.updateAdjacencyDatabase(extAdjDb, kTestingAreaName);
  EXPECT_THAT(update2.topologyChanged, IsTrue());
  ASSERT_THAT(update2.addedLinks, SizeIs(1));

  // Verify the link connects the correct nodes
  const std::shared_ptr<Link>& link = *update2.addedLinks.begin();
  EXPECT_THAT(link->getOtherNodeName("bbf01-ld001.dfw1"), Eq("eb01.iad1"));
  EXPECT_THAT(link->getOtherNodeName("eb01.iad1"), Eq("bbf01-ld001.dfw1"));
  EXPECT_THAT(link->getIfaceFromNode("eb01.iad1"), Eq("po1001"));
  EXPECT_THAT(link->getIfaceFromNode("bbf01-ld001.dfw1"), Eq("po1000"));
  EXPECT_THAT(link->getMetricFromNode("eb01.iad1"), Eq(10));
  EXPECT_THAT(link->getMetricFromNode("bbf01-ld001.dfw1"), Eq(10));
}

TEST_F(FabricHelperTestFixture, MaybeMakeLink_WithoutFabricHelper_NoLink) {
  //
  // Same topology as above but WITHOUT FabricHelper installed.
  // Since eb01.iad1 reports adjacency to "bbf01.dfw1" and there's no node
  // named "bbf01.dfw1", no link should be created.
  //

  thrift::Adjacency leafToExt = createAdjacency(
      "eb01.iad1", "po1000", "po1001", "fe80::1", "10.0.0.1", 10, 100);
  thrift::Adjacency extToFabric = createAdjacency(
      "bbf01.dfw1", "po1001", "po1000", "fe80::2", "10.0.0.2", 10, 200);

  thrift::AdjacencyDatabase leafAdjDb =
      createAdjDb("bbf01-ld001.dfw1", {leafToExt}, 1);
  thrift::AdjacencyDatabase extAdjDb =
      createAdjDb("eb01.iad1", {extToFabric}, 2);

  // No FabricHelper installed
  LinkState state{kTestingAreaName, "eb01.iad1"};

  state.updateAdjacencyDatabase(leafAdjDb, kTestingAreaName);
  LinkState::LinkStateChange update =
      state.updateAdjacencyDatabase(extAdjDb, kTestingAreaName);

  // No link should be created — "bbf01.dfw1" doesn't match any real node
  EXPECT_THAT(update.topologyChanged, IsFalse());
  EXPECT_THAT(update.addedLinks, IsEmpty());
}

TEST_F(FabricHelperTestFixture, MaybeMakeLink_LeafToSpine) {
  //
  // Topology: bbf01-ld001.dfw1 -- bbf01-sp001.dfw1
  //
  // Both sides report the correct node names directly (no fabric name
  // resolution needed). A bidirectional link should be created.
  //

  // bbf01-ld001.dfw1's adjacency to bbf01-sp001.dfw1
  thrift::Adjacency leafToSpine = createAdjacency(
      "bbf01-sp001.dfw1", "po10100", "po10200", "fe80::1", "10.0.0.1", 10, 100);

  // bbf01-sp001.dfw1's adjacency to bbf01-ld001.dfw1
  thrift::Adjacency spineToLeaf = createAdjacency(
      "bbf01-ld001.dfw1", "po10200", "po10100", "fe80::2", "10.0.0.2", 10, 200);

  thrift::AdjacencyDatabase leafAdjDb =
      createAdjDb("bbf01-ld001.dfw1", {leafToSpine}, 1);
  thrift::AdjacencyDatabase spineAdjDb =
      createAdjDb("bbf01-sp001.dfw1", {spineToLeaf}, 2);

  LinkState state{kTestingAreaName, "bbf01-ld001.dfw1"};
  state.addFabricHelper(fabricCfg_);

  // Update with leaf's adjDb first — no link yet (only one side present)
  LinkState::LinkStateChange update1 =
      state.updateAdjacencyDatabase(leafAdjDb, kTestingAreaName);
  EXPECT_THAT(update1.topologyChanged, IsFalse());

  // Update with spine's adjDb — bidirectional link should be created
  LinkState::LinkStateChange update2 =
      state.updateAdjacencyDatabase(spineAdjDb, kTestingAreaName);
  EXPECT_THAT(update2.topologyChanged, IsTrue());
  ASSERT_THAT(update2.addedLinks, SizeIs(1));

  // Verify the link connects the correct nodes with correct interfaces
  const std::shared_ptr<Link>& link = *update2.addedLinks.begin();
  EXPECT_THAT(
      link->getOtherNodeName("bbf01-ld001.dfw1"), Eq("bbf01-sp001.dfw1"));
  EXPECT_THAT(
      link->getOtherNodeName("bbf01-sp001.dfw1"), Eq("bbf01-ld001.dfw1"));
  EXPECT_THAT(link->getIfaceFromNode("bbf01-ld001.dfw1"), Eq("po10100"));
  EXPECT_THAT(link->getIfaceFromNode("bbf01-sp001.dfw1"), Eq("po10200"));
  EXPECT_THAT(link->getMetricFromNode("bbf01-ld001.dfw1"), Eq(10));
  EXPECT_THAT(link->getMetricFromNode("bbf01-sp001.dfw1"), Eq(10));
}

//
// getFabricMasterGenerator unit tests
//

TEST_F(FabricHelperTestFixture, GetFabricMasterGenerator_EmptyLinkMap) {
  FabricHelper helper = makeHelper();

  EXPECT_THAT(helper.getFabricMasterGenerator(), Eq(""));
}

TEST_F(FabricHelperTestFixture, TestGetFabricMasterGenerator) {
  // Topology:
  //   Fabric nodes: ld001 -- sp001, ld002 -- sp001 (connected)
  //                 sp002 (disconnected, lexicographically highest fabric node)
  //   Non-fabric nodes: eb01.rva1 -- eb01.ftw1 (connected, should be ignored)
  //
  // Expected: sp002 is skipped (disconnected), non-fabric nodes are ignored,
  //           sp001 is the highest connected fabric node.

  // Link: ld001 <-> sp001
  thrift::Adjacency ld001ToSp = createAdjacency(
      "bbf01-sp001.dfw1", "po10100", "po10200", "fe80::1", "10.0.0.1", 10, 0);
  thrift::Adjacency spToLd001 = createAdjacency(
      "bbf01-ld001.dfw1", "po10200", "po10100", "fe80::2", "10.0.0.2", 10, 0);
  std::shared_ptr<Link> link1 = std::make_shared<Link>(
      kTestingAreaName,
      "bbf01-ld001.dfw1",
      ld001ToSp,
      "bbf01-sp001.dfw1",
      spToLd001,
      true);

  // Link: ld002 <-> sp001
  thrift::Adjacency ld002ToSp = createAdjacency(
      "bbf01-sp001.dfw1", "po10101", "po10201", "fe80::3", "10.0.0.3", 10, 0);
  thrift::Adjacency spToLd002 = createAdjacency(
      "bbf01-ld002.dfw1", "po10201", "po10101", "fe80::4", "10.0.0.4", 10, 0);
  std::shared_ptr<Link> link2 = std::make_shared<Link>(
      kTestingAreaName,
      "bbf01-ld002.dfw1",
      ld002ToSp,
      "bbf01-sp001.dfw1",
      spToLd002,
      true);

  // Link: eb01.rva1 <-> eb01.ftw1 (non-fabric)
  thrift::Adjacency extAdj1 = createAdjacency(
      "eb01.ftw1", "po1000", "po1001", "fe80::5", "10.0.0.5", 10, 0);
  thrift::Adjacency extAdj2 = createAdjacency(
      "eb01.rva1", "po1001", "po1000", "fe80::6", "10.0.0.6", 10, 0);
  std::shared_ptr<Link> link3 = std::make_shared<Link>(
      kTestingAreaName, "eb01.rva1", extAdj1, "eb01.ftw1", extAdj2, true);

  linkMap_["bbf01-ld001.dfw1"].insert(link1);
  linkMap_["bbf01-ld002.dfw1"].insert(link2);
  linkMap_["bbf01-sp001.dfw1"].insert(link1);
  linkMap_["bbf01-sp001.dfw1"].insert(link2);
  linkMap_["bbf01-sp002.dfw1"] = {}; // disconnected
  linkMap_["eb01.rva1"].insert(link3);
  linkMap_["eb01.ftw1"].insert(link3);

  FabricHelper helper = makeHelper();

  // sp002 is highest but disconnected, non-fabric nodes ignored → sp001
  EXPECT_THAT(helper.getFabricMasterGenerator(), Eq("bbf01-sp001.dfw1"));
}

} // namespace
} // namespace openr

int
main(int argc, char* argv[]) {
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  return RUN_ALL_TESTS();
}
