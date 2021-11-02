/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <gtest/gtest.h>

#include <folly/Random.h>
#include <folly/gen/Base.h>
#include <folly/io/async/EventBase.h>
#include <openr/kvstore/Dual.h>

#include <vector>

using namespace openr;
using namespace folly;

// sync time-ms to wait for all DUAL nodes to converge
const std::chrono::milliseconds syncms{500};

// I/O response delay to mimic real senario
// e.g one link-up-event might result in different ack time on each end.
const uint32_t kIoDelayBaseMs{5};
const uint32_t kIoDelayVarMs{5};

struct Vertex {
  std::string name;
  bool up = true;
};

using Weight = int64_t;
struct Edge {
  std::string name1;
  std::string name2;
  Weight weight;
  bool up = true;
};

using Graph = boost::adjacency_list<
    boost::setS,
    boost::vecS,
    boost::undirectedS,
    Vertex,
    boost::property<boost::edge_weight_t, Weight>>;
using VertexDescriptor = boost::graph_traits<Graph>::vertex_descriptor;
using EdgeProperty = boost::property<boost::edge_weight_t, Weight>;
using IndexMap = boost::property_map<Graph, boost::vertex_index_t>::type;
using PredecessorMap = boost::iterator_property_map<
    VertexDescriptor*,
    IndexMap,
    VertexDescriptor,
    VertexDescriptor&>;
using DistanceMap =
    boost::iterator_property_map<Weight*, IndexMap, Weight, Weight&>;

using StatusStrings =
    std::pair<std::string, std::unordered_map<std::string, std::string>>;

// handy funtion to return a random delay in ms
uint32_t
randomDelayMs() {
  return kIoDelayBaseMs + folly::Random::rand32() % kIoDelayVarMs;
}

/**
 * check if a given graph is a spanning tree or not
 * - check all nodes are connected
 * - #edges == #nodes - 1
 */
bool
isSpt(const Graph& g) {
  auto numVertices = boost::num_vertices(g);
  if (numVertices < 1) {
    LOG(ERROR) << "numVertices: " << numVertices;
    return false;
  }
  auto numEdges = boost::num_edges(g);
  if (numEdges != numVertices - 1) {
    LOG(ERROR) << "numVertices: " << numVertices << ", numEdges: " << numEdges;
    return false;
  }

  std::vector<int> components(numVertices);
  auto numComponents = boost::connected_components(g, components.data());
  if (numComponents != 1) {
    LOG(ERROR) << "not a connected graph, num of components: " << numComponents;
    return false;
  }
  return true;
}

// validate route-info on a given dual node
// this ensures
// - node is in passive state
// - if distance is inf, then nexthop has to be none, and vice versa
bool
validateRouteInfo(const Dual::RouteInfo& info) {
  if (info.sm.state != DualState::PASSIVE) {
    LOG(ERROR) << "not in PASSIVE state";
    return false;
  }
  if (not info.nexthop.has_value()) {
    if (info.distance != std::numeric_limits<int64_t>::max()) {
      LOG(ERROR) << "nexthop: none but distance != intf";
      return false;
    }
  }
  if (info.distance == std::numeric_limits<int64_t>::max()) {
    if (info.nexthop.has_value()) {
      LOG(ERROR) << "distance: inf but nexthop: " << *info.nexthop;
      return false;
    }
  }
  return true;
}

// Test basic state machine logic to ensure state is transitioned properly
TEST(Dual, StateMachine) {
  DualStateMachine sm;
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  /**
   * passive state
   */
  // meet fc -> no state change
  sm.state = DualState::PASSIVE;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR);
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  // not meet fc
  sm.state = DualState::PASSIVE;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR, false);
  EXPECT_EQ(sm.state, DualState::ACTIVE3);

  sm.state = DualState::PASSIVE;
  sm.processEvent(DualEvent::OTHERS, false);
  EXPECT_EQ(sm.state, DualState::ACTIVE1);

  sm.state = DualState::PASSIVE;
  sm.processEvent(DualEvent::INCREASE_D, false);
  EXPECT_EQ(sm.state, DualState::ACTIVE1);

  /**
   * active0 state
   */
  sm.state = DualState::ACTIVE0;
  sm.processEvent(DualEvent::OTHERS);
  EXPECT_EQ(sm.state, DualState::ACTIVE0);

  sm.state = DualState::ACTIVE0;
  sm.processEvent(DualEvent::LAST_REPLY, true);
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  sm.state = DualState::ACTIVE0;
  sm.processEvent(DualEvent::LAST_REPLY, false);
  EXPECT_EQ(sm.state, DualState::ACTIVE2);

  sm.state = DualState::ACTIVE0;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR);
  EXPECT_EQ(sm.state, DualState::ACTIVE0);

  /**
   * active1 state
   */
  sm.state = DualState::ACTIVE1;
  sm.processEvent(DualEvent::INCREASE_D);
  EXPECT_EQ(sm.state, DualState::ACTIVE0);

  sm.state = DualState::ACTIVE1;
  sm.processEvent(DualEvent::LAST_REPLY);
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  sm.state = DualState::ACTIVE1;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR);
  EXPECT_EQ(sm.state, DualState::ACTIVE2);

  sm.state = DualState::ACTIVE1;
  sm.processEvent(DualEvent::OTHERS);
  EXPECT_EQ(sm.state, DualState::ACTIVE1);

  /**
   * active2 state
   */
  sm.state = DualState::ACTIVE2;
  sm.processEvent(DualEvent::OTHERS);
  EXPECT_EQ(sm.state, DualState::ACTIVE2);

  sm.state = DualState::ACTIVE2;
  sm.processEvent(DualEvent::LAST_REPLY, true);
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  sm.state = DualState::ACTIVE2;
  sm.processEvent(DualEvent::LAST_REPLY, false);
  EXPECT_EQ(sm.state, DualState::ACTIVE3);

  sm.state = DualState::ACTIVE2;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR);
  EXPECT_EQ(sm.state, DualState::ACTIVE2);

  /**
   * active3 state
   */
  sm.state = DualState::ACTIVE3;
  sm.processEvent(DualEvent::LAST_REPLY);
  EXPECT_EQ(sm.state, DualState::PASSIVE);

  sm.state = DualState::ACTIVE3;
  sm.processEvent(DualEvent::INCREASE_D);
  EXPECT_EQ(sm.state, DualState::ACTIVE2);

  sm.state = DualState::ACTIVE3;
  sm.processEvent(DualEvent::OTHERS);
  EXPECT_EQ(sm.state, DualState::ACTIVE3);

  sm.state = DualState::ACTIVE3;
  sm.processEvent(DualEvent::QUERY_FROM_SUCCESSOR);
  EXPECT_EQ(sm.state, DualState::ACTIVE3);
}

// Dual Implementation Test Node
class DualTestNode final : public DualNode {
 public:
  DualTestNode(
      const std::string& nodeId,
      bool isRoot,
      std::shared_ptr<folly::EventBase> evb,
      std::map<std::string, std::shared_ptr<DualTestNode>>& nodes)
      : DualNode(nodeId, isRoot), evb_(std::move(evb)), nodes_(nodes) {}

  bool
  sendDualMessages(
      const std::string& neighbor,
      const thrift::DualMessages& msgs) noexcept override {
    auto& otherNode = nodes_.at(neighbor);
    CHECK(neighborUp(neighbor))
        << nodeId << ": sending dual msgs to down neighbor " << neighbor;
    CHECK(msgs.messages_ref()->size()) << " send empty messages";

    evb_->runInEventBaseThread(
        [&, otherNode, msgs]() { otherNode->processDualMessages(msgs); });

    return true;
  }

  void
  processNexthopChange(
      const std::string& rootId,
      const std::optional<std::string>& oldNh,
      const std::optional<std::string>& newNh) noexcept override {
    std::string oldNhStr = oldNh.has_value() ? *oldNh : "none";
    std::string newNhStr = newNh.has_value() ? *newNh : "none";
    VLOG(1) << "node: " << nodeId << " at root: " << rootId << " nexthop"
            << " change " << oldNhStr << " -> " << newNhStr;
    return;
  }

  // event base loop
  std::shared_ptr<folly::EventBase> evb_;
  // reference to map<node-id: DualTestNode*>
  std::map<std::string, std::shared_ptr<DualTestNode>>& nodes_;
};

// Dual test fixture
class DualBaseFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    evb = std::make_shared<folly::EventBase>();
    thread = std::thread([&]() {
      VLOG(1) << "evb starting";
      evb->loopForever();
      VLOG(1) << "evb stopped";
    });
  }

  void
  TearDown() override {
    evb->terminateLoopSoon();
    thread.join();
    VLOG(1) << "evb thread stopped";
  }

  void
  addNode(const std::string& nodeId, bool isRoot) {
    auto node = std::make_shared<DualTestNode>(nodeId, isRoot, evb, nodes);
    nodes.emplace(nodeId, node);
    vertices.emplace_back(Vertex{nodeId, true});
    if (isRoot) {
      VLOG(1) << "add root " << nodeId;
      rootIds.emplace_back(nodeId);
    }
  }

  void
  addLink(const std::string& node1, const std::string& node2, int64_t cost) {
    for (const auto& edge : edges) {
      if ((edge.name1 == node1 and edge.name2 == node2) or
          (edge.name1 == node2 and edge.name2 == node1)) {
        return; // edge exist already
      }
    }
    edges.emplace_back(Edge{node1, node2, cost, false});
    peerUp(node1, node2, cost);
  }

  // trigger a link up event on both ends with random delay
  void
  peerUp(const std::string& node1, const std::string& node2, int64_t cost) {
    for (auto& edge : edges) {
      if ((edge.name1 == node1 and edge.name2 == node2) or
          (edge.name1 == node2 and edge.name2 == node1)) {
        if (edge.up) {
          // link already up
          return;
        }
        edge.up = true;
        break;
      }
    }
    evb->runInEventBaseThread([&, node1, node2, cost]() {
      evb->runAfterDelay(
          [&, node1, node2, cost]() { nodes.at(node1)->peerUp(node2, cost); },
          randomDelayMs());
      evb->runAfterDelay(
          [&, node1, node2, cost]() { nodes.at(node2)->peerUp(node1, cost); },
          randomDelayMs());
    });
  }

  // trigger a link down event on both ends with random delay
  void
  peerDown(const std::string& node1, const std::string& node2) {
    for (auto& edge : edges) {
      if ((edge.name1 == node1 and edge.name2 == node2) or
          (edge.name1 == node2 and edge.name2 == node1)) {
        if (not edge.up) {
          // link already down
          return;
        }
        edge.up = false;
        break;
      }
    }
    evb->runInEventBaseThread([&, node1, node2]() {
      evb->runAfterDelay(
          [&, node1, node2]() { nodes.at(node1)->peerDown(node2); },
          randomDelayMs());
      evb->runAfterDelay(
          [&, node1, node2]() { nodes.at(node2)->peerDown(node1); },
          randomDelayMs());
    });
  }

  // flap a link between node1 and node2
  void
  linkFlap(const std::string& node1, const std::string& node2, int64_t cost) {
    for (auto& edge : edges) {
      if ((edge.name1 == node1 and edge.name2 == node2) or
          (edge.name1 == node2 and edge.name2 == node1)) {
        if (not edge.up) {
          // link already down
          return;
        }
        break;
      }
    }
    evb->runInEventBaseThread([&, node1, node2, cost]() {
      // bring link down on both ends
      evb->runAfterDelay(
          [&, node1, node2]() { nodes.at(node1)->peerDown(node2); }, 0);
      evb->runAfterDelay(
          [&, node1, node2]() { nodes.at(node2)->peerDown(node1); }, 0);

      // bring them back up
      // Make sure link-up event is scheduled AFTER link-down event
      evb->runAfterDelay(
          [&, node1, node2, cost]() { nodes.at(node1)->peerUp(node2, cost); },
          randomDelayMs());
      evb->runAfterDelay(
          [&, node1, node2, cost]() { nodes.at(node2)->peerUp(node1, cost); },
          randomDelayMs());
    });
  }

  // trigger a node down event, all associated links will be brought down
  void
  nodeDown(const std::string& node) {
    for (auto& vertex : vertices) {
      if (vertex.name == node) {
        vertex.up = false;
        break;
      }
    }
    // bring corresponding edges down
    for (auto& edge : edges) {
      if (edge.name1 != node and edge.name2 != node) {
        continue; // link not affected
      }
      if (not edge.up) {
        continue;
      }
      peerDown(edge.name1, edge.name2);
    }
  }

  // trigger a node up event, all associated links will be brought up
  void
  nodeUp(const std::string& node) {
    for (auto& vertex : vertices) {
      if (vertex.name == node) {
        vertex.up = true;
        break;
      }
    }
    for (auto& edge : edges) {
      if (edge.name1 != node and edge.name2 != node) {
        continue; // link not affected
      }
      if (edge.up) {
        continue;
      }
      peerUp(edge.name1, edge.name2, edge.weight);
    }
  }

  // flap a node
  void
  nodeFlap(const std::string& node) {
    for (auto& edge : edges) {
      if (edge.name1 != node and edge.name2 != node) {
        continue; // link not affected
      }
      linkFlap(edge.name1, edge.name2, edge.weight);
    }
  }

  // print all nodes status according to given root
  static void
  printStatus(
      const std::unordered_map<std::string, StatusStrings>& status,
      std::optional<std::string> rootId = std::nullopt) {
    for (const auto& kv : status) {
      const auto& node = kv.first;
      // node level status
      const auto& nodeStatus = kv.second.first;
      // root level status
      const auto& rootsStatus = kv.second.second;
      if (rootId.has_value()) {
        // print specific root status
        if (rootsStatus.count(*rootId) == 0) {
          LOG(ERROR) << "node: " << node << " has no root " << *rootId
                     << "info";
          continue;
        }
        LOG(ERROR)
            << fmt::format("{}\n{}", nodeStatus, rootsStatus.at(*rootId));
      } else {
        // print all roots status
        std::vector<std::string> strs;
        for (const auto& rootStr : rootsStatus) {
          strs.emplace_back(rootStr.second);
        }
        LOG(ERROR)
            << fmt::format("{}\n{}", nodeStatus, folly::join("\n", strs));
      }
    }
  }

  // validate no-root case (no node declares itself as root)
  // if this case, we expect everynode reports a empty route-info map
  bool
  validateNoRoot(const std::unordered_map<
                 std::string,
                 std::unordered_map<std::string, Dual::RouteInfo>>& results) {
    for (const auto& kv : results) {
      const auto& node = kv.first;
      const auto& infos = kv.second;
      if (not infos.empty()) {
        LOG(ERROR) << node
                   << " has non-empty route-info map, size: " << infos.size();
        return false;
      }
    }
    return true;
  }

  // validate the correctness of Dual Algorithm for a given rootId
  // - ensure all nodes are in PASSIVE state
  // - ensure formed flooding topology is a SPT
  // - ensure flooding topology matches actual physical topology (spf)
  bool
  validateOnRoot(
      const std::string& rootId,
      const std::unordered_map<
          std::string,
          std::unordered_map<std::string, Dual::RouteInfo>>& results) {
    // construct flooding topology from results
    Graph floodTopo;
    std::unordered_map<std::string, VertexDescriptor> floodDescriptors;
    for (const auto& kv : results) {
      const auto& node = kv.first;
      if (kv.second.count(rootId) == 0) {
        LOG(ERROR) << node << " has no route-info for root " << rootId;
        return false;
      }
      const auto& info = kv.second.at(rootId);
      const auto& nexthop = info.nexthop;
      const auto& distance = info.distance;

      if (not validateRouteInfo(info)) {
        LOG(ERROR) << node << " route-info validation failed.";
        return false;
      }

      if (not nexthop.has_value() or
          distance == std::numeric_limits<int64_t>::max()) {
        // skip disconnected node
        continue;
      }

      // add node
      if (floodDescriptors.count(node) == 0) {
        auto d = boost::add_vertex(Vertex{node}, floodTopo);
        floodDescriptors.emplace(node, d);
      }

      // add node(nexthop)
      if (floodDescriptors.count(*nexthop) == 0) {
        auto d = boost::add_vertex(Vertex{*nexthop}, floodTopo);
        floodDescriptors.emplace(*nexthop, d);
      }

      // add edge
      if (*nexthop == node) {
        // skip root
        continue;
      }
      boost::add_edge(
          floodDescriptors.at(node),
          floodDescriptors.at(*nexthop),
          EdgeProperty(1),
          floodTopo);
    }

    // 1. validate flooding topology is a SPT
    if (not isSpt(floodTopo)) {
      LOG(ERROR) << "flooding topology is not a SPT";
      return false;
    }

    // construct physical topology from vertices and edges
    Graph physicalTopo;
    std::unordered_map<std::string, VertexDescriptor> physicalDescriptors;
    // add node
    for (const auto& vertex : vertices) {
      auto d = boost::add_vertex(vertex, physicalTopo);
      physicalDescriptors.emplace(vertex.name, d);
    }
    // add edge
    for (const auto& edge : edges) {
      if (not edge.up) {
        continue; // skip down edge
      }
      if (physicalDescriptors.count(edge.name1) == 0 or
          physicalDescriptors.count(edge.name2) == 0) {
        continue; // skip edge if any-end is down
      }
      boost::add_edge(
          physicalDescriptors.at(edge.name1),
          physicalDescriptors.at(edge.name2),
          EdgeProperty(edge.weight),
          physicalTopo);
    }

    // 2. validate flooding topology matches expected shortest-path graph
    // predecessors vector
    std::vector<VertexDescriptor> preds(boost::num_vertices(physicalTopo));
    // distances vector
    std::vector<Weight> dists(boost::num_vertices(physicalTopo));

    // build spt from this vertex
    IndexMap indexMap = boost::get(boost::vertex_index, physicalTopo);
    PredecessorMap predMap(preds.data(), indexMap);
    DistanceMap distMap(dists.data(), indexMap);
    boost::dijkstra_shortest_paths(
        physicalTopo,
        physicalDescriptors.at(rootId),
        boost::distance_map(distMap).predecessor_map(predMap));

    // verify number of connected nodes
    unsigned int numConnectedNodes = 0;
    for (const auto& weight : dists) {
      if (weight == std::numeric_limits<Weight>::max()) {
        continue;
      }
      ++numConnectedNodes;
    }
    if (numConnectedNodes != boost::num_vertices(floodTopo)) {
      LOG(ERROR)
          << "numConnectedNodes: " << numConnectedNodes << " != "
          << "num of flooding topology: " << boost::num_vertices(floodTopo);
      return false;
    }

    // verify distances
    for (const auto& kv : results) {
      const auto& node = kv.first;
      const auto& distance = kv.second.at(rootId).distance;
      const auto& expDistance = dists[physicalDescriptors.at(node)];
      if (expDistance != distance) {
        LOG(ERROR) << node << " distance: " << distance << " != "
                   << "expDistance: " << expDistance;
        return false;
      }
    }
    return true;
  }

  // validate the correctness of Dual Algorithm for all roots
  bool
  validate() {
    // get all-infos and stauts-strings
    std::unordered_map<
        std::string,
        std::unordered_map<std::string, Dual::RouteInfo>>
        infos;
    std::unordered_map<std::string, StatusStrings> status;
    getResults(infos, status);

    if (rootIds.empty()) {
      VLOG(1) << "validate no-root case";
      if (not validateNoRoot(infos)) {
        LOG(ERROR) << "validate no root failed";
        printStatus(status);
        return false;
      }
      return true;
    }

    for (const auto& rootId : rootIds) {
      VLOG(1) << "validate root " << rootId;
      if (not validateOnRoot(rootId, infos)) {
        LOG(ERROR) << "validate root " << rootId << " failed";
        printStatus(status, rootId);
        return false;
      }
    }
    return true;
  }

  // Single Link Failure Test
  // if not flap:
  // for EACH link: bring it down, wait-and-validate, bring it up,
  // wait-and-validate
  // if flap:
  // for EACH link: bring it down, bring it up right
  // away without delay, validate all
  bool
  singleLinkFailureTest(bool flap = false) {
    // flap each edge one by one and validate
    for (auto& edge : edges) {
      if (flap) {
        // flap link test
        VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2
                << ") flap";
        linkFlap(edge.name1, edge.name2, edge.weight);

        /* sleep override */
        std::this_thread::sleep_for(syncms);
        if (not validate()) {
          LOG(ERROR) << "flap link " << edge.name1 << ", " << edge.name2
                     << " validation failed";
          return false;
        }
        continue;
      }

      // bring edge down and validate
      VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2 << ") down";
      peerDown(edge.name1, edge.name2);

      /* sleep override */
      std::this_thread::sleep_for(syncms);
      if (not validate()) {
        LOG(ERROR) << "down link " << edge.name1 << ", " << edge.name2
                   << " validation failed";
        return false;
      }

      // bring edge back up and validate
      VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2 << ") up";
      peerUp(edge.name1, edge.name2, edge.weight);

      /* sleep override */
      std::this_thread::sleep_for(syncms);
      if (not validate()) {
        LOG(ERROR) << "up link " << edge.name1 << ", " << edge.name2
                   << " validation failed";
        return false;
      }
    }
    return true;
  }

  // Single Node Failure Test
  // if not flap:
  // for EACH node: bring it down, wait-and-validate, bring it up,
  // wait-and-validate
  // if flap:
  // for EACH node: bring it down, bring it up right
  // away without delay, validate all
  bool
  singleNodeFailureTest(bool flap = false) {
    // flap each node and validate
    for (auto& vertex : vertices) {
      if (vertex.name != "n0") {
        continue;
      }
      if (flap) {
        // flap node test
        VLOG(1) << "===> node (" << vertex.name << ") flap";
        nodeFlap(vertex.name);

        /* sleep override */
        std::this_thread::sleep_for(syncms);
        if (not validate()) {
          LOG(ERROR) << "flap node " << vertex.name << " validation failed";
          return false;
        }
        continue;
      }

      // bring node down and validate
      VLOG(1) << "===> node (" << vertex.name << ") down";
      nodeDown(vertex.name);

      /* sleep override */
      std::this_thread::sleep_for(syncms);
      if (not validate()) {
        LOG(ERROR) << "down node " << vertex.name << " validation failed";
        return false;
      }

      // bring node up and validate
      VLOG(1) << "===> node (" << vertex.name << ") up";
      nodeUp(vertex.name);
      /* sleep override */
      std::this_thread::sleep_for(syncms);
      if (not validate()) {
        LOG(ERROR) << "up node " << vertex.name << " validation failed";
        return false;
      }
    }
    return true;
  }

  // Multiple Failure Test
  // randomly pick 20% links, capped between [2, 6]
  // bring them down/up and validate, same flap logic as above
  bool
  multiFailureTest(bool flap = false) {
    // pick 20% of edges to shut down
    auto edgeNum = std::max(2, int(edges.size() * 0.2));
    edgeNum = std::min(edgeNum, 6);
    auto links = gen::from(edges) |
        gen::sample(edgeNum, folly::ThreadLocalPRNG()) |
        gen::as<std::vector<Edge>>();

    if (flap) {
      // flap test
      // flap all chosen edges
      for (const auto& edge : links) {
        VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2
                << ") flap";
        linkFlap(edge.name1, edge.name2, edge.weight);
      }

      /* sleep override */
      std::this_thread::sleep_for(syncms);
      if (not validate()) {
        LOG(ERROR) << "multi links flap validation failed";
        return false;
      }
      return true;
    }

    // bring down links
    for (const auto& edge : links) {
      VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2 << ") down";
      peerDown(edge.name1, edge.name2);
    }

    /* sleep override */
    std::this_thread::sleep_for(syncms);
    if (not validate()) {
      LOG(ERROR) << "multi links down validation failed";
      return false;
    }

    // bring links back up
    for (const auto& edge : links) {
      VLOG(1) << "===> link (" << edge.name1 << ", " << edge.name2 << ") up";
      peerUp(edge.name1, edge.name2, edge.weight);
    }

    // validate
    /* sleep override */
    std::this_thread::sleep_for(syncms);
    if (not validate()) {
      LOG(ERROR) << "multi links up validation failed";
      return false;
    }
    return true;
  }

  // collect results and status for all nodes (blocking call)
  // output: map<node-id: <root-id: RouteInfo>>
  // output: map<node-id: <root-id: status-string>>
  void
  getResults(
      std::unordered_map<
          std::string,
          std::unordered_map<std::string, Dual::RouteInfo>>& infos,
      std::unordered_map<std::string, StatusStrings>& status) {
    CHECK(infos.empty());
    CHECK(status.empty());

    for (const auto& kv : nodes) {
      auto& node = kv.second;
      evb->runInEventBaseThreadAndWait([&, node]() {
        infos.emplace(kv.first, node->getInfos());
        status.emplace(kv.first, node->getStatusStrings());
      });
    }
  }

  std::shared_ptr<folly::EventBase> evb;
  std::thread thread;
  // map<node-id: DualTestNode*>
  std::map<std::string, std::shared_ptr<DualTestNode>> nodes;
  std::vector<std::string> rootIds{};

  std::vector<Vertex> vertices;
  std::vector<Edge> edges;
};

// Test Parameters
struct TestParam {
  int totalRoots; // number of roots
  bool flap; // flap link/node or not
  TestParam(int totalRoots, bool flap) : totalRoots(totalRoots), flap(flap) {}
};

class DualFixture : public DualBaseFixture,
                    public ::testing::WithParamInterface<TestParam> {};

// Test all following different cases for each topology
INSTANTIATE_TEST_CASE_P(
    DualInstance,
    DualFixture,
    ::testing::Values(
        TestParam(0, false),
        TestParam(0, true),
        TestParam(1, false),
        TestParam(1, true),
        TestParam(2, false),
        TestParam(2, true)));

/**
 *  Circular Topology
 *  root: n0, each-link-cost: 1
 *  n0 --- n1
 *  |      |
 *  |     ...
 *  |      |
 * n(N-1)- n(N-2)
 */
TEST_P(DualFixture, CircularTest) {
  const auto& param = GetParam();
  const auto& totalRoots = param.totalRoots;
  const auto& flap = param.flap;
  VLOG(1) << "test params: " << totalRoots << ", " << flap;

  int numNodes = 4;
  // add nodes
  for (int i = 0; i < numNodes; ++i) {
    bool isRoot = i < totalRoots;
    addNode(fmt::format("n{}", i), isRoot);
  }
  // add links
  for (int i = 0; i < numNodes; ++i) {
    int j = (i + 1) % numNodes;
    addLink(fmt::format("n{}", i), fmt::format("n{}", j), 1);
  }

  /* sleep override */
  std::this_thread::sleep_for(syncms);
  EXPECT_TRUE(validate());

  EXPECT_TRUE(singleLinkFailureTest(flap));
  EXPECT_TRUE(singleNodeFailureTest(flap));
  EXPECT_TRUE(multiFailureTest(flap));
}

/**
 *  Fabric Topology (2 X 4)
 *  n0        n1
 *  |\        /\
 *  | \ ...  /  \
 *  |  \    /    \
 *  n2 n3 n4    n5
 */
TEST_P(DualFixture, FabricTest) {
  const auto& param = GetParam();
  const auto& totalRoots = param.totalRoots;
  const auto& flap = param.flap;
  VLOG(1) << "test params: " << totalRoots << ", " << flap;

  int m = 2;
  int n = 4;

  for (int i = 0; i < m + n; ++i) {
    bool isRoot = i < totalRoots;
    addNode(fmt::format("n{}", i), isRoot);
  }

  for (int i = 0; i < m; ++i) {
    for (int j = m; j < m + n; ++j) {
      addLink(fmt::format("n{}", i), fmt::format("n{}", j), 1);
    }
  }

  /* sleep override */
  std::this_thread::sleep_for(syncms);
  EXPECT_TRUE(validate());

  EXPECT_TRUE(singleLinkFailureTest(flap));
  EXPECT_TRUE(singleNodeFailureTest(flap));
  EXPECT_TRUE(multiFailureTest(flap));
}

/**
 *  Full-Mesh Topology
 */
TEST_P(DualFixture, FullMeshTest) {
  const auto& param = GetParam();
  const auto& totalRoots = param.totalRoots;
  const auto& flap = param.flap;
  VLOG(1) << "test params: " << totalRoots << ", " << flap;

  int numNodes = 4;
  // add nodes
  for (int i = 0; i < numNodes; ++i) {
    bool isRoot = i < totalRoots;
    addNode(fmt::format("n{}", i), isRoot);
  }

  // add links
  for (int i = 0; i < numNodes; ++i) {
    for (int j = 0; j < numNodes; ++j) {
      if (i == j) {
        continue;
      }
      addLink(fmt::format("n{}", i), fmt::format("n{}", j), 1);
    }
  }

  /* sleep override */
  std::this_thread::sleep_for(syncms);
  EXPECT_TRUE(validate());

  EXPECT_TRUE(singleLinkFailureTest(flap));
  EXPECT_TRUE(singleNodeFailureTest(flap));
  EXPECT_TRUE(multiFailureTest(flap));
}

/**
 * m X n Grid Topology
 */
TEST_P(DualFixture, GridTest) {
  const auto& param = GetParam();
  const auto& totalRoots = param.totalRoots;
  const auto& flap = param.flap;
  VLOG(1) << "test params: " << totalRoots << ", " << flap;

  int m = 2;
  int n = 3;
  // add nodes
  for (int i = 0; i < m * n; ++i) {
    bool isRoot = i < totalRoots;
    addNode(fmt::format("n{}", i), isRoot);
  }

  // add links
  for (int i = 0; i < m; ++i) {
    for (int j = 1; j < n; ++j) {
      addLink(
          fmt::format("n{}", i * n + j), fmt::format("n{}", i * n + j - 1), 1);
    }
  }
  for (int i = 1; i < m; ++i) {
    for (int j = 0; j < n; ++j) {
      addLink(
          fmt::format("n{}", i * n + j), fmt::format("n{}", i * n + j - n), 1);
    }
  }

  /* sleep override */
  std::this_thread::sleep_for(syncms);
  EXPECT_TRUE(validate());

  EXPECT_TRUE(singleLinkFailureTest(flap));
  EXPECT_TRUE(singleNodeFailureTest(flap));
  EXPECT_TRUE(multiFailureTest(flap));
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
