/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <openr/common/LsdbUtil.h>
#include <openr/decision/Link.h>

using namespace testing;
using namespace openr;

namespace openr {
namespace {

const std::string kArea = "area1";

TEST(LinkTest, StringConstructor) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  EXPECT_THAT(link.getArea(), Eq(kArea));
  EXPECT_THAT(link.getOtherNodeName("node1"), Eq("node2"));
  EXPECT_THAT(link.getOtherNodeName("node2"), Eq("node1"));
  EXPECT_THAT(link.getIfaceFromNode("node1"), Eq("if1"));
  EXPECT_THAT(link.getIfaceFromNode("node2"), Eq("if2"));
  EXPECT_THAT(link.isUp(), IsTrue());
  EXPECT_THAT(link.getUsability(), IsTrue());
  EXPECT_THAT(link.getMetricFromNode("node1"), Eq(1));
  EXPECT_THAT(link.getMetricFromNode("node2"), Eq(1));
  EXPECT_THAT(link.getMaxMetric(), Eq(1));
}

TEST(LinkTest, StringConstructor_NotUsable) {
  Link link(kArea, "node1", "if1", "node2", "if2", false);

  EXPECT_THAT(link.isUp(), IsFalse());
  EXPECT_THAT(link.getUsability(), IsFalse());
}

TEST(LinkTest, AdjacencyConstructor) {
  thrift::Adjacency adj1 =
      createAdjacency("node1", "if1", "if2", "fe80::1", "10.0.0.1", 10, 100);
  thrift::Adjacency adj2 =
      createAdjacency("node2", "if2", "if1", "fe80::2", "10.0.0.2", 20, 200);

  Link link(kArea, "node1", adj1, "node2", adj2);

  EXPECT_THAT(link.getArea(), Eq(kArea));
  EXPECT_THAT(link.getIfaceFromNode("node1"), Eq("if1"));
  EXPECT_THAT(link.getIfaceFromNode("node2"), Eq("if2"));
  EXPECT_THAT(link.getMetricFromNode("node1"), Eq(10));
  EXPECT_THAT(link.getMetricFromNode("node2"), Eq(20));
  EXPECT_THAT(link.getMaxMetric(), Eq(20));
  EXPECT_THAT(link.getAdjLabelFromNode("node1"), Eq(100));
  EXPECT_THAT(link.getAdjLabelFromNode("node2"), Eq(200));
  EXPECT_THAT(link.isUp(), IsTrue());
}

TEST(LinkTest, OrderedNames) {
  Link link1(kArea, "bbb", "if1", "aaa", "if2");
  EXPECT_THAT(link1.firstNodeName(), Eq("aaa"));
  EXPECT_THAT(link1.secondNodeName(), Eq("bbb"));

  Link link2(kArea, "aaa", "if1", "bbb", "if2");
  EXPECT_THAT(link2.firstNodeName(), Eq("aaa"));
  EXPECT_THAT(link2.secondNodeName(), Eq("bbb"));
}

TEST(LinkTest, InvalidNodeName) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  EXPECT_THROW(link.getOtherNodeName("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getIfaceFromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getMetricFromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getAdjLabelFromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getWeightFromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getOverloadFromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getNhV4FromNode("unknown"), std::invalid_argument);
  EXPECT_THROW(link.getNhV6FromNode("unknown"), std::invalid_argument);
}

TEST(LinkTest, SetMetric) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  EXPECT_THAT(link.setMetricFromNode("node1", 50), IsTrue());
  EXPECT_THAT(link.getMetricFromNode("node1"), Eq(50));
  EXPECT_THAT(link.getMetricFromNode("node2"), Eq(1));
  EXPECT_THAT(link.getMaxMetric(), Eq(50));

  EXPECT_THAT(link.setMetricFromNode("node2", 100), IsTrue());
  EXPECT_THAT(link.getMetricFromNode("node2"), Eq(100));
  EXPECT_THAT(link.getMaxMetric(), Eq(100));

  EXPECT_THROW(link.setMetricFromNode("unknown", 10), std::invalid_argument);
}

TEST(LinkTest, SetOverload) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  EXPECT_THAT(link.isUp(), IsTrue());

  // Overloading one side brings the link down
  EXPECT_THAT(link.setOverloadFromNode("node1", true), IsTrue());
  EXPECT_THAT(link.isUp(), IsFalse());
  EXPECT_THAT(link.getOverloadFromNode("node1"), IsTrue());
  EXPECT_THAT(link.getOverloadFromNode("node2"), IsFalse());

  // Setting overload again doesn't change topology (link already down)
  EXPECT_THAT(link.setOverloadFromNode("node2", true), IsFalse());
  EXPECT_THAT(link.isUp(), IsFalse());

  // Removing one overload doesn't bring link up (other side still overloaded)
  EXPECT_THAT(link.setOverloadFromNode("node1", false), IsFalse());
  EXPECT_THAT(link.isUp(), IsFalse());

  // Removing last overload brings link up
  EXPECT_THAT(link.setOverloadFromNode("node2", false), IsTrue());
  EXPECT_THAT(link.isUp(), IsTrue());

  EXPECT_THROW(
      link.setOverloadFromNode("unknown", true), std::invalid_argument);
}

TEST(LinkTest, SetAdjLabel) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  link.setAdjLabelFromNode("node1", 42);
  EXPECT_THAT(link.getAdjLabelFromNode("node1"), Eq(42));
  EXPECT_THAT(link.getAdjLabelFromNode("node2"), Eq(0));

  link.setAdjLabelFromNode("node2", 99);
  EXPECT_THAT(link.getAdjLabelFromNode("node2"), Eq(99));

  EXPECT_THROW(link.setAdjLabelFromNode("unknown", 1), std::invalid_argument);
}

TEST(LinkTest, SetWeight) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  link.setWeightFromNode("node1", 1000);
  EXPECT_THAT(link.getWeightFromNode("node1"), Eq(1000));

  link.setWeightFromNode("node2", 2000);
  EXPECT_THAT(link.getWeightFromNode("node2"), Eq(2000));

  EXPECT_THROW(link.setWeightFromNode("unknown", 1), std::invalid_argument);
}

TEST(LinkTest, SetLinkUsability) {
  Link link1(kArea, "node1", "if1", "node2", "if2", true);
  Link link2(kArea, "node1", "if1", "node2", "if2", false);

  EXPECT_THAT(link1.isUp(), IsTrue());
  // Changing usability from true to false returns true (topology changed)
  EXPECT_THAT(link1.setLinkUsability(link2), IsTrue());
  EXPECT_THAT(link1.isUp(), IsFalse());

  // Setting same usability returns false (no change)
  EXPECT_THAT(link1.setLinkUsability(link2), IsFalse());
}

TEST(LinkTest, Equality) {
  Link link1(kArea, "node1", "if1", "node2", "if2");
  Link link2(kArea, "node1", "if1", "node2", "if2");
  Link link3(kArea, "node1", "if1", "node3", "if3");

  EXPECT_THAT(link1 == link2, IsTrue());
  EXPECT_THAT(link1 == link3, IsFalse());

  // Equality is based on ordered names, not metrics
  link1.setMetricFromNode("node1", 999);
  EXPECT_THAT(link1 == link2, IsTrue());
}

TEST(LinkTest, LessThan) {
  Link link1(kArea, "aaa", "if1", "bbb", "if2");
  Link link2(kArea, "ccc", "if1", "ddd", "if2");

  // Different hashes → compared by hash
  if (link1.hash != link2.hash) {
    EXPECT_THAT(link1 < link2, Eq(link1.hash < link2.hash));
  }

  // Same link → not less than itself
  EXPECT_THAT(link1 < link1, IsFalse());
}

TEST(LinkTest, Hash) {
  Link link1(kArea, "node1", "if1", "node2", "if2");
  Link link2(kArea, "node1", "if1", "node2", "if2");
  Link link3(kArea, "node1", "if1", "node3", "if3");

  // Same links have same hash
  EXPECT_THAT(link1.hash, Eq(link2.hash));
  // Order shouldn't matter
  Link link4(kArea, "node2", "if2", "node1", "if1");
  EXPECT_THAT(link1.hash, Eq(link4.hash));

  // Different links (likely) have different hashes
  EXPECT_THAT(link1.hash, Ne(link3.hash));

  // std::hash produces same result
  size_t stdHash = std::hash<Link>()(link1);
  EXPECT_THAT(stdHash, Eq(link1.hash));
}

TEST(LinkTest, ToString) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  std::string str = link.toString();
  EXPECT_THAT(str, HasSubstr("node1"));
  EXPECT_THAT(str, HasSubstr("node2"));
  EXPECT_THAT(str, HasSubstr("if1"));
  EXPECT_THAT(str, HasSubstr("if2"));
}

TEST(LinkTest, DirectionalToString) {
  Link link(kArea, "node1", "if1", "node2", "if2");

  std::string str = link.directionalToString("node1");
  EXPECT_THAT(str, HasSubstr("node1"));
  EXPECT_THAT(str, HasSubstr("node2"));
}

TEST(LinkTest, LinkPtrHelpers) {
  std::shared_ptr<Link> link1 =
      std::make_shared<Link>(kArea, "node1", "if1", "node2", "if2");
  std::shared_ptr<Link> link2 =
      std::make_shared<Link>(kArea, "node1", "if1", "node2", "if2");
  std::shared_ptr<Link> link3 =
      std::make_shared<Link>(kArea, "node3", "if3", "node4", "if4");

  Link::LinkPtrHash hasher;
  EXPECT_THAT(hasher(link1), Eq(link1->hash));

  Link::LinkPtrEqual equal;
  EXPECT_THAT(equal(link1, link2), IsTrue());
  EXPECT_THAT(equal(link1, link3), IsFalse());

  Link::LinkPtrLess less;
  EXPECT_THAT(less(link1, link2), IsFalse());
}

TEST(LinkTest, LinkSet) {
  std::shared_ptr<Link> link1 =
      std::make_shared<Link>(kArea, "node1", "if1", "node2", "if2");
  std::shared_ptr<Link> link2 =
      std::make_shared<Link>(kArea, "node1", "if1", "node2", "if2");
  std::shared_ptr<Link> link3 =
      std::make_shared<Link>(kArea, "node3", "if3", "node4", "if4");

  Link::LinkSet set;
  set.insert(link1);
  set.insert(link2); // duplicate, should not increase size
  EXPECT_THAT(set, SizeIs(1));

  set.insert(link3);
  EXPECT_THAT(set, SizeIs(2));
}

TEST(LinkTest, LinkSetHash) {
  std::shared_ptr<Link> link1 =
      std::make_shared<Link>(kArea, "node1", "if1", "node2", "if2");
  std::shared_ptr<Link> link2 =
      std::make_shared<Link>(kArea, "node3", "if3", "node4", "if4");

  Link::LinkSet setA;
  setA.insert(link1);
  setA.insert(link2);

  Link::LinkSet setB;
  setB.insert(link2);
  setB.insert(link1);

  // Same elements in different order → same hash (XOR is commutative)
  size_t hashA = std::hash<Link::LinkSet>()(setA);
  size_t hashB = std::hash<Link::LinkSet>()(setB);
  EXPECT_THAT(hashA, Eq(hashB));

  // equal_to should also match
  EXPECT_THAT(std::equal_to<Link::LinkSet>()(setA, setB), IsTrue());

  Link::LinkSet setC;
  setC.insert(link1);
  EXPECT_THAT(std::equal_to<Link::LinkSet>()(setA, setC), IsFalse());
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
