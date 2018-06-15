/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DecisionOld.h"

#include <chrono>
#include <string>

#include <boost/functional/hash.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/property_map/property_map.hpp>
#include <fbzmq/service/logging/LogSample.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <gflags/gflags.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Util.h>

using namespace std;

using apache::thrift::FRAGILE;

using Weight = uint64_t;
using Graph = boost::adjacency_list<
    boost::listS,
    boost::vecS,
    boost::directedS,
    boost::no_property,
    boost::property<boost::edge_weight_t, Weight>>;
using VertexDescriptor = boost::graph_traits<Graph>::vertex_descriptor;
using EdgeDescriptor = boost::graph_traits<Graph>::edge_descriptor;
using EdgeProperty = boost::property<boost::edge_weight_t, Weight>;
using IndexMap = boost::property_map<Graph, boost::vertex_index_t>::type;
using WeightMap = boost::property_map<Graph, boost::edge_weight_t>;
using PredecessorMap = boost::iterator_property_map<
    VertexDescriptor*,
    IndexMap,
    VertexDescriptor,
    VertexDescriptor&>;
using DistanceMap =
    boost::iterator_property_map<Weight*, IndexMap, Weight, Weight&>;

namespace openr {

namespace {

using NexthopAdj = std::pair<thrift::Adjacency, Weight>;

// Default HWM is 1k. We set it to 0 to buffer all received messages.
const int kStoreSubReceiveHwm{0};

struct NodeData {
  std::string nodeName;

  bool isOverloaded{false};

  int32_t nodeLabel{0};

  unordered_map<
      std::pair<
          std::string /* remoteIfName */,
          std::string /* remoteNodeName */>,
      thrift::Adjacency>
      adjDb;

  std::unordered_set<thrift::IpPrefix> prefixes;

  bool
  isAdjacent(std::string const& otherIfName, std::string const& otherNodeName) {
    return adjDb.count({otherIfName, otherNodeName});
  }

  bool
  isNeighbor(std::string const& otherNodeName) {
    for (auto const& kv : adjDb) {
      auto const& neighbor = kv.first.second;
      if (neighbor == otherNodeName) {
        return true;
      }
    }
    return false;
  }

  // Update adjDb, at the same time check if drain status of router/adjacency
  // changes, if so return true to trigger SPF recalculation
  bool
  updateAdjacencyDb(
      thrift::AdjacencyDatabase adjacencyDb,
      std::vector<std::tuple<std::string, std::string, std::string>>& toUpdate,
      std::vector<std::tuple<std::string, std::string, std::string>>& toDel) {
    DCHECK(nodeName == adjacencyDb.thisNodeName)
        << "call updateAdjacencyDb on wrong node!";
    // Check if there's any node/adjcency overload, if so trigger SPF
    bool triggerSpf = (isOverloaded != adjacencyDb.isOverloaded) or
      (nodeLabel != adjacencyDb.nodeLabel);

    std::unordered_set<
        std::pair<std::string /* remote interface */, std::string /* node */>>
        newAdj;

    for (auto const& adj : adjacencyDb.adjacencies) {
      auto const& remoteIfName = getRemoteIfName(adj);
      newAdj.emplace(std::make_pair(remoteIfName, adj.otherNodeName));

      // if already in adjDb, check if drain status changed
      if (isAdjacent(remoteIfName, adj.otherNodeName)) {
        VLOG(3) << nodeName << " and neighbor " << adj.otherNodeName
                << " is verified adjacent on remote IfName " << remoteIfName;
        auto& it = adjDb.at(std::make_pair(remoteIfName, adj.otherNodeName));
        if (it != adj) {
          // Update cached adjDb
          VLOG(3) << "Adjacency exsists and is found updated";
          it = adj;
          toUpdate.push_back(
            std::make_tuple(adj.ifName, adj.otherIfName, adj.otherNodeName));
        }
      } else {
        // Update cached adjDb
        VLOG(3) << "Adjacency between " << nodeName
                << " and " << adj.otherNodeName
                << " on interface " << remoteIfName << " is freshly inserted";
        adjDb.emplace(std::make_pair(remoteIfName, adj.otherNodeName), adj);
        toUpdate.push_back(
          std::make_tuple(adj.ifName, adj.otherIfName, adj.otherNodeName));
      }
    }

    for (auto it = adjDb.begin(); it != adjDb.end();) {
      if (newAdj.count(it->first) == 0) {
        toDel.push_back(std::make_tuple(
          it->second.ifName, it->second.otherIfName, it->second.otherNodeName));
        // Update cahced adjDb
        it = adjDb.erase(it);
      } else {
        ++it;
      }
    }

    // update AdjacencyDatabase attributes
    isOverloaded = adjacencyDb.isOverloaded;
    nodeLabel = adjacencyDb.nodeLabel;
    return triggerSpf;
  }

  bool
  updatePrefixDb(std::unordered_set<thrift::IpPrefix> prefixDb) {
    bool prefixChange = false;
    // check if there's any prefix to be removed
    for (auto const& prefix : prefixes) {
      if (prefixDb.count(prefix) == 0) {
        prefixChange = true;
        break;
      }
    }
    // check if there's any new prefix to be added
    for (auto const& prefix : prefixDb) {
      if (prefixes.count(prefix) == 0) {
        prefixChange = true;
        break;
      }
    }

    // Update cached prefixes
    prefixes = prefixDb;
    return prefixChange;
  }
};

/**
 * Create a route with single path.
 */
folly::Optional<thrift::Route>
createRoute(
    const thrift::IpPrefix& prefix,
    const thrift::Adjacency& adjacency,
    Weight metric,
    bool enableV4) {
  auto addr = toIPAddress(prefix.prefixAddress);
  if (addr.isV4() and !enableV4) {
    LOG(ERROR) << "Received v4 prefix while v4 is not enabled.";
    return nullptr;
  }

  thrift::Path path(
      FRAGILE,
      addr.isV4() ? adjacency.nextHopV4 : adjacency.nextHopV6,
      adjacency.ifName,
      metric);
  return thrift::Route(FRAGILE, prefix, {std::move(path)});
}

/**
 * Create multi-path route, prefix with set of loop-free next-hops. If any
 * error occurs then the returned value will be empty and error will be logged.
 */
folly::Optional<thrift::Route>
createRouteMulti(
    const thrift::IpPrefix& prefix,
    const vector<pair<thrift::Adjacency, Weight>>& adjacencies,
    bool enableV4) {
  auto addr = toIPAddress(prefix.prefixAddress);
  if (addr.isV4() and !enableV4) {
    LOG(ERROR) << "Received v4 prefix while v4 is not enabled.";
    return nullptr;
  }

  if (adjacencies.empty()) {
    return nullptr;
  }

  std::set<thrift::Path> paths;
  for (auto const& kv : adjacencies) {
    auto const& adjacency = kv.first;
    paths.insert(thrift::Path(
        FRAGILE,
        addr.isV4() ? adjacency.nextHopV4 : adjacency.nextHopV6,
        adjacency.ifName,
        kv.second /* metric */));
  }

  return thrift::Route(
      FRAGILE, prefix, vector<thrift::Path>(paths.begin(), paths.end()));
}

} // anonymous namespace

/**
 * Private implementation of the SpfSolverOld
 */
class SpfSolverOld::SpfSolverOldImpl {
 public:
  explicit SpfSolverOldImpl(bool enableV4) : enableV4_(enableV4) {}

  ~SpfSolverOldImpl() = default;

  bool updateAdjacencyDatabase(thrift::AdjacencyDatabase const& adjacencyDb);
  bool deleteAdjacencyDatabase(const std::string& nodeName);
  std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
  getAdjacencyDatabases();
  bool updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb);
  bool deletePrefixDatabase(const std::string& nodeName);
  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases();

  thrift::RouteDatabase buildShortestPaths(const std::string& myNodeName);
  thrift::RouteDatabase buildMultiPaths(const std::string& myNodeName);
  thrift::RouteDatabase buildRouteDb(const std::string& myNodeName);

  /**
   * Graph is prepared as followed
   * 1. Add all node vertices (adjDb_.keys())
   * 2. Use adjDb to add edges between nodes. bidirectional check is
   *    enforced here.
   *
   * Bidirectional Check: An edge between node will be considered only when
   * both nodes reports each other. Otherwise it is ignored.
   *
   * Prepare data structures that we use for SPF run and it can be re-used
   * across multiple SPF computations.
   */
  void prepareGraph();

  std::unordered_map<std::string, int64_t> getCounters();

  fbzmq::ThreadData&
  getThreadData() noexcept {
    return tData_;
  }

 private:
  // no copy
  SpfSolverOldImpl(SpfSolverOldImpl const&) = delete;
  SpfSolverOldImpl& operator=(SpfSolverOldImpl const&) = delete;

  // run SPF and produce map from node name to next-hop and metric to reach it
  unordered_map<
      string /* other node name */,
      pair<string /* next-hop node name */, Weight /* metric */>>
  runSpf(const std::string& myNodeName);

  // bidirectionally verify newly added/deleted interfaces
  // When adjacency update is received, we enforce the bidirectional check and
  // only trigger SPF re-calculation when there's actual link change.
  // For example, for a graph like:
  // node1: intf1 -------- node2: intf2
  // When we firstly receive adjacency update from node1 declaring that node2
  // is added at local interface intf1, we don't trigger SPF immediately
  // because node1 --- node2 is not fully brought up yet. We only trigger SPF
  // when node2 also updates its adjacency with nodes1 added at node2's
  // local interface intf2.
  // Similarly, when the link is brought down, we trigger SPF recalculation
  // immediately when node1 is declaring node2 is no longer its neighbor,
  // and we don't trigger spf caclulation again when node2 reports node1 as
  // down on it's intf2 following previous event as the adjacency is no
  // longer bi-directional
  bool bidirectionalAdjacencyCheck(
      const std::string& nodeName,
      const std::vector<std::tuple<
          std::string /* local intf */,
          std::string /* remote intf */,
          std::string /* remote node */>>& toUpdate,
      const std::vector<std::tuple<
          std::string /* local intf */,
          std::string /* remote intf */,
          std::string /* remote node */>>& toDel);

  // this is the new data structure to support adjacencyDb updates, prefix
  // update and corresponding bidirectional check
  std::unordered_map<std::string /* nodeName */, NodeData> nodeData_;

  // the graph we operate on for SPF
  Graph graph_;

  // helpers for mapping router names to descriptros and vice versa
  unordered_map<string /* router name */, VertexDescriptor> nameToDescriptor_;
  unordered_map<VertexDescriptor, string /* router name */> descriptorToName_;

  // compare two adjacencies
  struct AdjComparator {
    bool
    operator()(
        const thrift::Adjacency& lhs, const thrift::Adjacency& rhs) const {
      return lhs.metric < rhs.metric;
    }
  };

  // compare two next-hop adjacencies
  struct NexthopAdjComparator {
    bool
    operator()(const NexthopAdj& lhs, const NexthopAdj& rhs) const {
      return lhs.first.metric < rhs.first.metric;
    }
  };

  // Adjacencies indexed by originating router and destination router
  // Note: use multiset, not set, since there are duplicates, i.e., adjacencies
  // with same metric
  unordered_map<
      pair<string /* thisRouterName */, string /* otherRouterName */>,
      multiset<
          thrift::Adjacency,
          AdjComparator> /* adjacencies ordered by non-descreasing metric */>
      adjacencyIndex_;

  // Save all direct next-hop distance from a given source node to a destination
  // node. We update it as we compute all LFA routes from perspective of source
  std::unordered_map<
      std::string /* node name */,
      std::unordered_map<
          std::string /* destination node */,
          std::map<
              std::string /* nexthop node */,
              Weight /* metric value from source node to destination node */>>>
      allDistFromNode_;

  // track some stats
  fbzmq::ThreadData tData_;

  // is v4 enabled. If yes then DecisionOld will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};
};

bool
SpfSolverOld::SpfSolverOldImpl::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& adjacencyDb) {
  auto const& nodeName = adjacencyDb.thisNodeName;
  VLOG(2) << "Updating adjacency database for node " << nodeName;
  tData_.addStatValue("decision.adj_db_update", 1, fbzmq::COUNT);

  for (auto const& adj : adjacencyDb.adjacencies) {
    VLOG(3) << "  nbr: " << adj.otherNodeName << ", remoteIfName: "
            << getRemoteIfName(adj)
            << ", ifName: " << adj.ifName << ", metric: " << adj.metric
            << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt;
  }

  // Insert if not exists
  if (nodeData_.count(nodeName) == 0) {
    // This is a new node inserted in nodeData.adjDb, create entry in adjDb
    NodeData newNode;
    newNode.nodeName = adjacencyDb.thisNodeName;
    newNode.isOverloaded = adjacencyDb.isOverloaded;
    newNode.nodeLabel = adjacencyDb.nodeLabel;
    nodeData_.emplace(nodeName, newNode);
  }

  std::vector<std::tuple<
      std::string /* local intf */,
      std::string /* remote intf */,
      std::string /* remote node */>> toUpdate;
  std::vector<std::tuple<
      std::string /* local intf */,
      std::string /* remote intf */,
      std::string /* remote node */>> toDel;
  // Check if there's any node/adjcency overload, if so trigger SPF
  // updateAdjacencyDb only returns true if both ends of the adjacencies are
  // bidirectionally verified
  if (nodeData_.at(nodeName).updateAdjacencyDb(adjacencyDb, toUpdate, toDel)) {
    VLOG(3) << "Node label or overload attribute changed for " << nodeName;
    return true;
  }

  // If nothing changed, no need to trigger SPF
  if (toUpdate.empty() and toDel.empty()) {
    VLOG(3) << "Nothing to update in adjDb";
    return false;
  }

  return bidirectionalAdjacencyCheck(nodeName, toUpdate, toDel);
}

bool
SpfSolverOld::SpfSolverOldImpl::bidirectionalAdjacencyCheck(
    const std::string& nodeName,
    const std::vector<std::tuple<
        std::string /* local intf */,
        std::string /* remote intf */,
        std::string /* remote node */>>& toUpdate,
    const std::vector<std::tuple<
        std::string /* local intf */,
        std::string /* remote intf */,
        std::string /* remote node */>>& toDel) {
  // Check if newly added adjacencies is bidirectional. If so, trigger SPF
  for (auto const& adj : toUpdate) {
    auto const& ifName = std::get<0>(adj);
    auto const& otherIfName = std::get<1>(adj);
    auto const& otherNodeName = std::get<2>(adj);
    if (nodeData_.count(otherNodeName) == 0) {
      continue;
    }
    auto otherNodeIt = nodeData_.find(otherNodeName);
    // For newly added adjacencies, we only re-caculate SPF when current
    // node is also present in adjDb of added adjacencies
    // otherwise this adjacency update is not bidirectional verified
    // SPF won't get affected
    if (otherNodeIt->second.isAdjacent(ifName, nodeName)) {
      VLOG(2) << "Adding adjacency between " << nodeName << " and other node "
              << otherNodeName << " is bidirectionally verified on interface "
              << ifName;
      return true;
    }
    // if otherIfName is empty, we do loose bidirectional check:
    // if otherNodeName is neighbor of myNodeName, we assume this is a
    // bidirectional verified neighbor
    if (otherIfName.empty() && otherNodeIt->second.isNeighbor(nodeName)) {
      VLOG(2) << "Adding adjacency between " << nodeName << " and other node "
              << otherNodeName << " is bidirectionally verified as neighbors";
      return true;
    }
  }

  for (auto const& adj : toDel) {
    auto const& ifName = std::get<0>(adj);
    auto const& otherIfName = std::get<1>(adj);
    auto const& otherNodeName = std::get<2>(adj);
    if (nodeData_.count(otherNodeName) == 0) {
      continue;
    }
    auto otherNodeIt = nodeData_.find(otherNodeName);
    // For deleted adjacencies, we only re-caculated SPF when current node
    // is still present in adjDb of any of deleted adjacencies
    if (otherNodeIt->second.isAdjacent(ifName, nodeName)) {
      VLOG(2) << "Deleting adjacency between " << nodeName << " and other node "
              << otherNodeName << " is bidirectionally verified on interface "
              << ifName;
      return true;
    }
    // The following is to make sure to keep backward compatibility with
    // previous version of openr, in which remote interface is not specified in
    // newly discovered neighbor.
    // if remote interface is not specified, we do loose bidirectional check:
    // myNodeName is already discovered as the neighbor of remote node,
    // we assume this is a bidirectional verified neighbor
    if (otherIfName.empty() && otherNodeIt->second.isNeighbor(nodeName)) {
      VLOG(2) << "Deleting adjacency between " << nodeName << " and other node "
              << otherNodeName << " is bidirectionally verified as neighbors";
      return true;
    }
  }

  return false;
}

bool
SpfSolverOld::SpfSolverOldImpl::deleteAdjacencyDatabase(const std::string& nodeName) {
  auto nodeIt = nodeData_.find(nodeName);

  if (nodeIt == nodeData_.end()) {
    LOG(WARNING) << "Trying to delete adjacency db for nonexisting node "
                 << nodeName;
    return false;
  }
  nodeIt->second.adjDb.clear();
  // reset
  nodeIt->second.isOverloaded = false;
  nodeIt->second.nodeLabel = 0;
  if (nodeIt->second.adjDb.empty() and nodeIt->second.prefixes.empty()) {
    nodeData_.erase(nodeName);
  }
  return true;
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
SpfSolverOld::SpfSolverOldImpl::getAdjacencyDatabases() {
  std::unordered_map<std::string, thrift::AdjacencyDatabase> adjacencyDbs;
  for (auto const& kv : nodeData_) {
    auto const& nodeName = kv.first;
    auto const& nodeInfo = kv.second;
    thrift::AdjacencyDatabase adjDb;
    adjDb.thisNodeName = nodeInfo.nodeName;
    adjDb.isOverloaded = nodeInfo.isOverloaded;
    adjDb.nodeLabel = nodeInfo.nodeLabel;
    for (auto const& adj : nodeInfo.adjDb) {
      adjDb.adjacencies.emplace_back(adj.second);
    }
    adjacencyDbs.emplace(nodeName, adjDb);
  }
  return adjacencyDbs;
}

bool
SpfSolverOld::SpfSolverOldImpl::updatePrefixDatabase(
    thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;
  VLOG(2) << "Updating prefix database for node " << nodeName;
  tData_.addStatValue("decision.prefix_db_update", 1, fbzmq::COUNT);

  std::unordered_set<thrift::IpPrefix> prefixes;
  for (auto const& prefixEntry : prefixDb.prefixEntries) {
    prefixes.insert(prefixEntry.prefix);
  }

  if (nodeData_.count(nodeName) == 0) {
    NodeData newNode;
    newNode.nodeName = nodeName;
    newNode.prefixes = prefixes;
    nodeData_.emplace(nodeName, newNode);
    // When prefix-db is added before adjacency-db, we won't have any route
    // to other nodes even after SPF computation
    return false;
  }

  return nodeData_.at(nodeName).updatePrefixDb(prefixes);
}

bool
SpfSolverOld::SpfSolverOldImpl::deletePrefixDatabase(const std::string& nodeName) {
  auto nodeIt = nodeData_.find(nodeName);
  if (nodeIt == nodeData_.end()) {
    LOG(INFO) << "Trying to delete prefix db for nonexisting node " << nodeName;
    return false;
  }
  if (nodeIt->second.prefixes.empty()) {
    LOG(INFO) << "Trying to delete empty prefix db for node " << nodeName;
    return false;
  }

  nodeIt->second.prefixes.clear();
  if (nodeIt->second.adjDb.empty() and nodeIt->second.prefixes.empty()) {
    nodeData_.erase(nodeName);
  }
  return true;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolverOld::SpfSolverOldImpl::getPrefixDatabases() {
  // prefixes per advertising router
  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
      prefixDbs;

  // construct prefix dbs from nodeData_.prefixes
  for (auto const& kv : nodeData_) {
    const auto& nodeName = kv.first;
    const auto& prefixes = kv.second.prefixes;
    auto& prefixDb = prefixDbs[nodeName];
    prefixDb.thisNodeName = nodeName;
    for (auto const& prefix : prefixes) {
      prefixDb.prefixEntries.emplace_back(
          apache::thrift::FRAGILE, prefix, thrift::PrefixType::LOOPBACK, "");
    }
  }

  return prefixDbs;
}

void
SpfSolverOld::SpfSolverOldImpl::prepareGraph() {
  VLOG(2) << "SpfSolverOld::SpfSolverOldImpl::prepareGraph() called.";

  graph_.clear();
  nameToDescriptor_.clear();
  descriptorToName_.clear();
  adjacencyIndex_.clear();

  // Create a set of all unidirectional adjacencies, which is from one node to
  // another one connected thru one remote interface
  std::unordered_set<std::tuple<
      std::string /* myNodeName*/,
      std::string /* otherNodeName */,
      std::string /* otherIfName*/>>
      presentAdjacencies;

  // Create a set of all unidirectional adjacencies, just for those with old
  // version of spark payload to maintain backward compatibility
  // TODO: remove logic related to this data structure in the next release
  std::unordered_set<
      std::pair<std::string /* myNodeName*/, std::string /* otherNodeName */>>
      compatiblePresentAdjacencies;

  for (auto const& kv : nodeData_) {
    auto const& thisNodeName = kv.first;
    auto const& adjacencies = kv.second.adjDb;

    for (auto const& adjacency : adjacencies) {
      // Skip adjacency from SPF graph if it is overloaded to avoid transit
      // traffic through it.
      if (adjacency.second.isOverloaded) {
        continue;
      }

      if (adjacency.second.otherIfName.empty() or
          adjacency.second.otherIfName.find("neigh-") == 0) {
        // maintain backward compatibility
        compatiblePresentAdjacencies.insert(
            std::make_pair(thisNodeName, adjacency.second.otherNodeName));
      } else {
        presentAdjacencies.insert(std::make_tuple(
            thisNodeName,
            adjacency.second.otherNodeName,
            adjacency.second.otherIfName));
      }
    }
  }

  // Build adjacencyIndex_ with bidirectional check enforced
  for (auto const& kv : nodeData_) {
    auto const& thisNodeName = kv.first;
    auto const& adjacencies = kv.second.adjDb;

    // walk all my ajdacencies
    for (auto const& myAdjacency : adjacencies) {
      auto const& otherNodeName = myAdjacency.second.otherNodeName;
      auto const& thisIfName = myAdjacency.second.ifName;

      // Skip if overloaded
      if (myAdjacency.second.isOverloaded) {
        continue;
      }

      // Check for reverse adjacency
      bool isBidir = presentAdjacencies.count(
          std::make_tuple(otherNodeName, thisNodeName, thisIfName));
      bool compatibleIsBidir = compatiblePresentAdjacencies.count(
          std::make_pair(otherNodeName, thisNodeName));
      if (!isBidir and !compatibleIsBidir) {
        VLOG(2) << "Failed to find matching adjacency for `" << thisNodeName
                << "' in `" << otherNodeName
                << "' thru interface " << thisIfName;
        continue;
      }

      adjacencyIndex_[{thisNodeName, otherNodeName}].insert(myAdjacency.second);
    } // for adjDb
  } // for nodeData_

  // 1. Add all node vertices
  for (auto const& kv : nodeData_) {
    auto const& nodeName = kv.first;
    auto const& descriptor = boost::add_vertex(graph_);
    nameToDescriptor_[nodeName] = descriptor;
    descriptorToName_[descriptor] = nodeName;
  }

  // 2. Add edges between node vertices
  for (const auto& kv : adjacencyIndex_) {
    const auto& thisNodeName = kv.first.first;
    const auto& otherNodeName = kv.first.second;
    const auto& adjacencies = kv.second;

    // this will not throw by virtue of building the name to descriptor map.
    // bidirectional check is enforced
    auto const& srcVertex = nameToDescriptor_.at(thisNodeName);
    auto const& dstVertex = nameToDescriptor_.at(otherNodeName);

    // Is thisNode overloaded. If node is overloaded then we bump up metric
    // values of it's outgoing edges by `kOverloadNodeMetric`. We use this
    // information later on to deduce if a shortest path is going over
    // overloaded node or not.
    // NOTE: link's metric can never be greater than kOverloadNodeMetric
    uint64_t metric = adjacencies.begin()->metric;
    if (nodeData_.at(thisNodeName).isOverloaded) {
      metric += Constants::kOverloadNodeMetric;
    }

    // for two nodes with multiple adjacencies in between, use the min metric
    // as their edge metric
    boost::add_edge(srcVertex, dstVertex, EdgeProperty(metric), graph_);
  } // for adjacencyIndex_

} // prepareGraph

/**
 * Compute shortest-path routes from perspective of myNodeName; NOTE: you
 * need to call prepareGraph() to initialize the graph structure before
 * calling this method.
 */
unordered_map<
    string /* otherVertexName (node) */,
    pair<string /* nextHopVertexName (node) */, Weight>>
SpfSolverOld::SpfSolverOldImpl::runSpf(const std::string& myNodeName) {
  tData_.addStatValue("decision.spf_runs", 1, fbzmq::COUNT);

  // map of other nodes, next-hops and distances to reach to them
  unordered_map<
      string /* otherVertexName */,
      pair<string /* nextHopVertexName */, Weight>>
      result;

  // Find our node-vertex-descriptor before proceeding
  auto it = nameToDescriptor_.find(myNodeName);
  if (it == nameToDescriptor_.end()) {
    LOG(ERROR) << "Could not find descriptor for router `" << myNodeName << "`";
    return result;
  }
  auto const& myVertex = it->second;

  // Refer here for example of BGL Dijkstra use:
  // http://programmingexamples.net/wiki/Boost/BGL/DijkstraComputePath

  // holds predecessors for vertices in shortest-path tree
  // i.e. indexed by vertex and gives you the vertex that
  // precedes this one on the path toward the SPT root
  vector<VertexDescriptor> pred(boost::num_vertices(graph_));

  // distances from each vertex to the root of SPT
  vector<Weight> dist(boost::num_vertices(graph_));

  // build SPT from this vertex
  IndexMap indexMap = boost::get(boost::vertex_index, graph_);
  PredecessorMap predMap(&pred[0], indexMap);
  DistanceMap distMap(&dist[0], indexMap);
  boost::dijkstra_shortest_paths(
      graph_, myVertex, boost::distance_map(distMap).predecessor_map(predMap));

  // walk all the nodes in the topology and compute paths to their prefixes
  for (auto const& kv : nameToDescriptor_) {
    auto const& otherVertexName = kv.first;
    auto const& otherVertex = kv.second;

    // skip self
    if (otherVertexName == myNodeName) {
      continue;
    }

    // indirectly connected, trace the path
    auto prevVertex = otherVertex;
    auto currVertex = pred[otherVertex];

    if (currVertex == prevVertex) {
      // unreachable from the root of the SPT
      continue;
    }

    while (currVertex != myVertex) {
      prevVertex = currVertex;
      currVertex = pred[currVertex];
    }

    // at this point prevVertex -> to our nextHop
    auto nextHopVertexName = descriptorToName_[prevVertex];
    result[otherVertexName] = {nextHopVertexName, dist[otherVertex]};
  } // for nameToDescriptor_

  // Print SPF table for debugging
  VLOG(4) << "SPF table for " << myNodeName;
  for (auto const& kv : result) {
    auto const& otherVertexName = kv.first;
    auto const& nextHopVertexName = kv.second.first;
    auto const& weight = kv.second.second;
    VLOG(4) << folly::sformat(
        "  {} via {}, cost {}", otherVertexName, nextHopVertexName, weight);
  }

  return result;
}

thrift::RouteDatabase
SpfSolverOld::SpfSolverOldImpl::buildShortestPaths(const std::string& myNodeName) {
  VLOG(4) << "SpfSolverOld::buildShortestPaths for " << myNodeName;

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  if (nodeData_.count(myNodeName) == 0) {
    LOG(ERROR) << "Couldn't find node-database for myself: `" << myNodeName
               << "`, skipping spf run";
    return routeDb;
  }

  tData_.addStatValue("decision.paths_build_requests", 1, fbzmq::COUNT);
  const auto startTime = std::chrono::steady_clock::now();

  prepareGraph();
  auto spfMap = runSpf(myNodeName);
  const bool myselfOverloaded = nodeData_.at(myNodeName).isOverloaded;

  // Load balance traffic to a given prefix advertised from two or more nodes
  // Combine nexthops towards all nodes bounded to the same prefix and pick
  // the smallest from them for loop-free routing
  unordered_map<
      thrift::IpPrefix, /* prefix */
      multiset<
          std::pair<thrift::Adjacency, Weight>,
          NexthopAdjComparator>
          /* adjacencies ordered by non-descreasing metric */>
      prefixToNextHops;

  for (auto const& kv : nodeData_) {
    auto const& thisNodeName = kv.first;
    if (thisNodeName == myNodeName) {
      continue;
    }
    // Find minimum nexthop to reach that node
    auto nodeIt = spfMap.find(thisNodeName);
    if (nodeIt == spfMap.end()) {
      VLOG(2) << "Skipping unreachable node " << thisNodeName;
      continue;
    }

    auto const& nextHopNodeName = nodeIt->second.first;
    auto metric = nodeIt->second.second;

    // Subtract overload metric value if myself is overloaded
    if (myselfOverloaded) {
      CHECK_LT(Constants::kOverloadNodeMetric, metric);
      metric -= Constants::kOverloadNodeMetric;
    }

    // Skip route if it is going through a node which is overloaded
    if (metric > Constants::kOverloadNodeMetric) {
      VLOG(2) << "Skipping shortest route to node " << thisNodeName
              << " because it is via an overloaded node.";
      continue;
    }

    // Add route for each prefix bounded to this node via the nextHopNodeName
    auto const& nhAdjs = adjacencyIndex_.at({myNodeName, nextHopNodeName});
    for (auto const& prefix : kv.second.prefixes) {
      prefixToNextHops[prefix].insert(std::make_pair(*nhAdjs.begin(), metric));
    }
  } // for nodeData_

  for (auto const& kv : prefixToNextHops) {
    auto const& prefix = kv.first;
    // pick up nexthop with minimum metric
    auto const& nhAdj = *kv.second.begin();
    auto route =
        createRoute(prefix, nhAdj.first, nhAdj.second /* metric */, enableV4_);
    if (route) {
      routeDb.routes.emplace_back(std::move(*route));
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "DecisionOld::buildShortestPaths took " << deltaTime.count()
            << "ms.";
  tData_.addStatValue(
      "decision.spf.shortestpath_ms", deltaTime.count(), fbzmq::AVG);

  return routeDb;
} // buildShortestPaths

thrift::RouteDatabase
SpfSolverOld::SpfSolverOldImpl::buildMultiPaths(const std::string& myNodeName) {
  VLOG(4) << "SpfSolverOld::buildMultiPaths for " << myNodeName;

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  if (nodeData_.count(myNodeName) == 0) {
    LOG(ERROR) << "Couldn't find node-database for myself: " << myNodeName
               << ", skipping multi-spf run";
    return routeDb;
  }

  tData_.addStatValue("decision.paths_build_requests", 1, fbzmq::COUNT);
  auto const& startTime = std::chrono::steady_clock::now();

  // reset next-hops info
  std::unordered_map<
    std::string /* destination-node */,
    std::map<
        std::string /* nexthop node */,
        Weight /* metric value from nexthop-node to destination-node */>>
    nodeDist;

  prepareGraph();
  auto prepareTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "DecisionOld::prepareGraph took " << prepareTime.count() << "ms.";

  auto mySpfPaths = runSpf(myNodeName);

  // avoid duplicate iterations over a neighbor which can happen due to multiple
  // adjacencies to it
  std::unordered_set<std::string /* adjacent node name */> visitedAdjNodes;

  // Go over all neighbors and find shortest distance of prefixes from them
  for (auto const& adj : nodeData_.at(myNodeName).adjDb) {
    auto const& adjNodeName = adj.second.otherNodeName;

    // Skip if already visited before
    if (not visitedAdjNodes.insert(adjNodeName).second) {
      VLOG(4) << "Adjacent neighbor " << adjNodeName << " has been considered.";
      continue;
    }

    // Skip if we don't have reverse edge from adjNodeName->myNodeName
    if (!adjacencyIndex_.count({adjNodeName, myNodeName})) {
      VLOG(2) << "Neighbor " << adjNodeName << " doesn't have reverse edge "
              << "to myself " << myNodeName << ". Skipping spf run for it.";
      continue;
    }

    // Update cost from this node to itself
    nodeDist[adjNodeName][adjNodeName] = 0;

    // Run SPF for the node
    VLOG(4) << "Running SPF for neighbor: " << adjNodeName;
    auto nbrSpfPaths = runSpf(adjNodeName);

    // Walk through all nodes and find shortest distance from myNodeName
    // to the prefix via adjNodeName
    for (auto const& kv : nodeData_) {
      auto const& thisNodeName = kv.first;
      if (thisNodeName == myNodeName) {
        continue;
      }

      // Skip if node is unreachable from neighbor
      auto nodeIt = nbrSpfPaths.find(thisNodeName);
      if (nodeIt == nbrSpfPaths.end()) {
        continue; // Node is unreachable from neighbor
      }

      // Get distances from adjNodeName
      auto adjNodeDist = nodeIt->second.second;
      auto adjMyNodeDist = nbrSpfPaths.at(myNodeName).second;

      // Get optimal distance of thisNodeName from us. Node must be
      // reachable from us because it is reachable from our neighbor
      auto myNodeDist = mySpfPaths.at(thisNodeName).second;

      // A neighbor (N) can provide loop free alternate to destination (D) from
      // source (S) if and only if
      //   Distance_opt(N, D) < Distance_opt(N, S) + Distance_opt(S, D)
      // For more info read: https://tools.ietf.org/html/rfc5286
      // NOTE: We are using negated condition here to skip the neighbor
      if (adjNodeDist >= adjMyNodeDist + myNodeDist) {
        VLOG(4) << "Skipping non LFA nexthop " << adjNodeName << " to "
                << "node " << thisNodeName;
        continue;
      }

      VLOG(4) << "Adding LFA nexthop " << adjNodeName << " for node "
              << thisNodeName;
      nodeDist[thisNodeName][adjNodeName] = adjNodeDist;
    } // nodeData_
  } // nodeData_[myNodeName].adjDb

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "DecisionOld::buildMultiPaths took " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.spf.multipath_ms", deltaTime.count(), fbzmq::AVG);

  // Update allDistFromNode_ for myNodeName
  allDistFromNode_[myNodeName] = nodeDist;

  return buildRouteDb(myNodeName);

} // buildMultiPaths

thrift::RouteDatabase
SpfSolverOld::SpfSolverOldImpl::buildRouteDb(const std::string& myNodeName) {
  VLOG(4) << "SpfSolverOld::buildRouteDb for " << myNodeName;

  tData_.addStatValue("decision.route_build_requests", 1, fbzmq::COUNT);

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  if (allDistFromNode_.count(myNodeName) == 0) {
    LOG(ERROR) << "Could not find adjacency Db for myself: " << myNodeName
               << ", skipping multi-spf run";
    return routeDb;
  }

  const auto& nodeDist = allDistFromNode_.at(myNodeName);

  const auto startTime = std::chrono::steady_clock::now();
  // Build RouteDb for all prefixes including LFAs
  unordered_map<
      thrift::IpPrefix, /* prefix */
      vector<std::pair<thrift::Adjacency, Weight>>>
      prefixToNextHops;
  for (auto const& kv : nodeData_) {
    auto const& nodeName = kv.first;

    // skip our own prefixes
    if (nodeName == myNodeName) {
      continue;
    }

    auto nodeIt = nodeDist.find(nodeName);
    if (nodeIt == nodeDist.end()) {
      VLOG(4) << "Skipping route to unreachable node " << nodeName;
      continue;
    }

    std::vector<std::pair<thrift::Adjacency, Weight>> adjacencies;
    for (auto const& nhMetric : nodeIt->second) {
      auto const& nhNodeName = nhMetric.first;
      auto metric = nhMetric.second;

      // Skip route if it is going through a node which is overloaded
      // NOTE: the metric here is a distance to prefix-node via our neighbor
      if (metric > Constants::kOverloadNodeMetric) {
        VLOG(2) << "Skipping shortest route to node " << nodeName
                << " because it is via an overloaded node.";
        continue;
      }

      // Add all adjacencies to nextHopNodeName as potential nexthops. FIB will
      // only program ones with min metric and others will act as LFAs
      for (const auto& adj : adjacencyIndex_.at({myNodeName, nhNodeName})) {
        adjacencies.push_back({adj, metric + adj.metric});
      }
    }

    for (auto const& prefix : kv.second.prefixes) {
      // skip routeDb update when a prefix is advertised from more than one node
      if (nodeData_.at(myNodeName).prefixes.count(prefix)) {
        VLOG(2) << "Prefix " << toString(prefix) << "is bouned to myself "
                << myNodeName << ", skipping routeDb update....";
        continue;
      }
      prefixToNextHops[prefix].insert(
          prefixToNextHops[prefix].end(),
          adjacencies.begin(),
          adjacencies.end());
    }
  } // for nodeData_ (build RouteDb)

  for (auto const& kv : prefixToNextHops) {
    auto const& prefix = kv.first;
    auto const& adjs = kv.second;
    auto route = createRouteMulti(prefix, adjs, enableV4_);
    if (route) {
      routeDb.routes.emplace_back(std::move(*route));
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "DecisionOld::buildRouteDb took " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.spf.buildroute_ms", deltaTime.count(), fbzmq::AVG);

  return routeDb;
} // buildRouteDb

std::unordered_map<std::string, int64_t>
SpfSolverOld::SpfSolverOldImpl::getCounters() {
  return tData_.getCounters();
}

//
// Public SpfSolverOld
//

SpfSolverOld::SpfSolverOld(bool enableV4)
    : impl_(new SpfSolverOld::SpfSolverOldImpl(enableV4)) {}

SpfSolverOld::~SpfSolverOld() {}

// update adjacencies for the given router; everything is replaced
bool
SpfSolverOld::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& adjacencyDb) {
  return impl_->updateAdjacencyDatabase(adjacencyDb);
}

bool
SpfSolverOld::deleteAdjacencyDatabase(const std::string& nodeName) {
  return impl_->deleteAdjacencyDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
SpfSolverOld::getAdjacencyDatabases() {
  return impl_->getAdjacencyDatabases();
}

// update prefixes for a given router
bool
SpfSolverOld::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  return impl_->updatePrefixDatabase(prefixDb);
}

bool
SpfSolverOld::deletePrefixDatabase(const std::string& nodeName) {
  return impl_->deletePrefixDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolverOld::getPrefixDatabases() {
  return impl_->getPrefixDatabases();
}

thrift::RouteDatabase
SpfSolverOld::buildShortestPaths(const std::string& myNodeName) {
  return impl_->buildShortestPaths(myNodeName);
}

thrift::RouteDatabase
SpfSolverOld::buildMultiPaths(const std::string& myNodeName) {
  return impl_->buildMultiPaths(myNodeName);
}

thrift::RouteDatabase
SpfSolverOld::buildRouteDb(const std::string& myNodeName) {
  return impl_->buildRouteDb(myNodeName);
}

std::unordered_map<std::string, int64_t>
SpfSolverOld::getCounters() {
  return impl_->getCounters();
}

//
// DecisionOld class implementation
//

DecisionOld::DecisionOld(
    std::string myNodeName,
    bool enableV4,
    const AdjacencyDbMarker& adjacencyDbMarker,
    const PrefixDbMarker& prefixDbMarker,
    std::chrono::milliseconds debounceMinDur,
    std::chrono::milliseconds debounceMaxDur,
    const KvStoreLocalCmdUrl& storeCmdUrl,
    const KvStoreLocalPubUrl& storePubUrl,
    const DecisionCmdUrl& decisionCmdUrl,
    const DecisionPubUrl& decisionPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : processUpdatesBackoff_(debounceMinDur, debounceMaxDur),
      myNodeName_(myNodeName),
      adjacencyDbMarker_(adjacencyDbMarker),
      prefixDbMarker_(prefixDbMarker),
      storeCmdUrl_(storeCmdUrl),
      storePubUrl_(storePubUrl),
      decisionCmdUrl_(decisionCmdUrl),
      decisionPubUrl_(decisionPubUrl),
      storeSub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      decisionRep_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      decisionPub_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}) {
  processUpdatesTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { processPendingUpdates(); });
  spfSolver_ = std::make_unique<SpfSolverOld>(enableV4);

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Initialize ZMQ sockets
  prepare(zmqContext);
}

void
DecisionOld::prepare(fbzmq::Context& zmqContext) noexcept {
  VLOG(2) << "Decision: Binding cmdUrl '" << decisionCmdUrl_ << "'";
  const auto repBind = decisionRep_.bind(fbzmq::SocketUrl{decisionCmdUrl_});
  if (repBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << decisionCmdUrl_ << "' "
               << repBind.error();
  }

  VLOG(2) << "Decision: Binding pubUrl '" << decisionPubUrl_ << "'";
  const auto pubBind = decisionPub_.bind(fbzmq::SocketUrl{decisionPubUrl_});
  if (pubBind.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << decisionPubUrl_ << "' "
               << pubBind.error();
  }

  VLOG(2) << "Decision: Connecting to store '" << storePubUrl_ << "'";
  const auto optRet =
      storeSub_.setSockOpt(ZMQ_RCVHWM, &kStoreSubReceiveHwm, sizeof(int));
  if (optRet.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_RCVHWM to " << kStoreSubReceiveHwm << " "
               << optRet.error();
  }
  const auto subConnect = storeSub_.connect(fbzmq::SocketUrl{storePubUrl_});
  if (subConnect.hasError()) {
    LOG(FATAL) << "Error binding to URL '" << storePubUrl_ << "' "
               << subConnect.error();
  }
  const auto subRet = storeSub_.setSockOpt(ZMQ_SUBSCRIBE, "", 0);
  if (subRet.hasError()) {
    LOG(FATAL) << "Error setting ZMQ_SUBSCRIBE to "
               << ""
               << " " << subRet.error();
  }

  VLOG(2) << "DecisionOld thread attaching socket/event callbacks...";

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Attach callback for processing publications on storeSub_ socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*storeSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "DecisionOld: publication received...";

        auto maybeThriftPub = storeSub_.recvThriftObj<thrift::Publication>(
            serializer_);
        if (maybeThriftPub.hasError()) {
          LOG(ERROR) << "Error processing KvStore publication: "
                     << maybeThriftPub.error();
          return;
        }

        // Apply publication and update stored update status
        auto const& res = processPublication(maybeThriftPub.value());
        processUpdatesStatus_.adjChanged |= res.adjChanged;
        processUpdatesStatus_.prefixesChanged |= res.prefixesChanged;
        // compute routes with exponential backoff timer if needed
        if (res.adjChanged || res.prefixesChanged) {
          if (!processUpdatesBackoff_.atMaxBackoff()) {
            processUpdatesBackoff_.reportError();
            processUpdatesTimer_->scheduleTimeout(
                processUpdatesBackoff_.getTimeRemainingUntilRetry());
          } else {
            CHECK(processUpdatesTimer_->isScheduled());
          }
        }
      });

  // Attach callback for processing requests on REP socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*decisionRep_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "DecisionOld: request received...";
        processRequest();
      });

  auto zmqContextPtr = &zmqContext;
  scheduleTimeout(
      std::chrono::milliseconds(500),
      [this, zmqContextPtr] { initialSync(*zmqContextPtr); }
  );
}

void
DecisionOld::processRequest() {
  auto maybeThriftReq = decisionRep_.recvThriftObj<thrift::DecisionRequest>(
      serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "DecisionOld: Error processing request on REP socket: "
               << maybeThriftReq.error();
    return;
  }

  auto thriftReq = maybeThriftReq.value();
  thrift::DecisionReply reply;
  switch (thriftReq.cmd) {
  case thrift::DecisionCommand::ROUTE_DB_GET: {
    auto nodeName = thriftReq.nodeName;
    if (nodeName.empty()) {
      VLOG(1) << "DecisionOld: Routes requested with no specific node name. "
              << "Returning " << myNodeName_ << " routes.";
      nodeName = myNodeName_;
    }

    reply.routeDb = spfSolver_->buildMultiPaths(nodeName);
    break;
  }

  case thrift::DecisionCommand::ADJ_DB_GET: {
    reply.adjDbs = spfSolver_->getAdjacencyDatabases();
    break;
  }

  case thrift::DecisionCommand::PREFIX_DB_GET: {
    reply.prefixDbs = spfSolver_->getPrefixDatabases();
    break;
  }

  default: {
    LOG(ERROR) << "Unexpected command received: "
               << folly::get_default(
                      thrift::_DecisionCommand_VALUES_TO_NAMES,
                      thriftReq.cmd,
                      "UNKNOWN");
    return;
  }
  }

  auto sendRc = decisionRep_.sendThriftObj(reply, serializer_);
  if (sendRc.hasError()) {
    LOG(ERROR) << "Error sending response: " << sendRc.error();
  }
}

std::unordered_map<std::string, int64_t>
DecisionOld::getCounters() {
  return spfSolver_->getCounters();
}

ProcessPublicationResultOld
DecisionOld::processPublication(thrift::Publication const& thriftPub) {
  ProcessPublicationResultOld res;

  // LSDB addition/update
  // deserialize contents of every LSDB key

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals.empty() and thriftPub.expiredKeys.empty()) {
    return res;
  }

  for (const auto& kv : thriftPub.keyVals) {
    const auto& key = kv.first;
    const auto& rawVal = kv.second;
    std::string prefix, nodeName;
    folly::split(Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

    if (not rawVal.value.hasValue()) {
      // skip TTL update
      DCHECK(rawVal.ttlVersion > 0);
      continue;
    }

    try {
      if (key.find(adjacencyDbMarker_) == 0) {
        // update adjacencyDb
        auto adjacencyDb =
            fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
                rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, adjacencyDb.thisNodeName);
        if (spfSolver_->updateAdjacencyDatabase(adjacencyDb)) {
          res.adjChanged = true;
          pendingAdjUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
        }
        continue;
      }

      if (key.find(prefixDbMarker_) == 0) {
        // update prefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, prefixDb.thisNodeName);
        if (spfSolver_->updatePrefixDatabase(prefixDb)) {
          res.prefixesChanged = true;
          pendingPrefixUpdates_.addUpdate(myNodeName_, prefixDb.perfEvents);
        }
        continue;
      }
    } catch (std::exception const& e) {
      LOG(ERROR) << "Failed to deserialize info for key " << key
                 << ". Exception: " << folly::exceptionStr(e);
    }
  }

  // LSDB deletion
  for (const auto& key : thriftPub.expiredKeys) {
    std::string prefix, nodeName;
    folly::split(Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

    if (key.find(adjacencyDbMarker_) == 0) {
      if (spfSolver_->deleteAdjacencyDatabase(nodeName)) {
        res.adjChanged = true;
        pendingAdjUpdates_.addUpdate(myNodeName_, folly::none);
      }
      continue;
    }

    if (key.find(prefixDbMarker_) == 0) {
      if (spfSolver_->deletePrefixDatabase(nodeName)) {
        res.prefixesChanged = true;
        pendingPrefixUpdates_.addUpdate(myNodeName_, folly::none);
      }
      continue;
    }
  }

  return res;
}

// perform full dump of all LSDBs and run initial routing computations
void
DecisionOld::initialSync(fbzmq::Context& zmqContext) {
  thrift::Request thriftReq;

  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> storeReq(zmqContext);

  // we'll be using this to get the full dump from the KvStore
  const auto reqConnect = storeReq.connect(fbzmq::SocketUrl{storeCmdUrl_});
  if (reqConnect.hasError()) {
    LOG(FATAL) << "Error connecting to URL '" << storeCmdUrl_ << "' "
               << reqConnect.error();
  }

  thriftReq.cmd = thrift::Command::KEY_DUMP;
  storeReq.sendThriftObj(thriftReq, serializer_);

  VLOG(2) << "DecisionOld process requesting initial state...";

  // receive the full dump of the database
  auto maybeThriftPub = storeReq.recvThriftObj<thrift::Publication>(
      serializer_, Constants::kReadTimeout);
  if (maybeThriftPub.hasError()) {
    LOG(ERROR) << "Error processing KvStore publication: "
               << maybeThriftPub.error();
    return;
  }

  // Process publication and immediately apply updates
  auto const& ret = processPublication(maybeThriftPub.value());
  if (ret.adjChanged) {
    // Graph changes
    processPendingAdjUpdates();
  } else if (ret.prefixesChanged) {
    // Only Prefix changes, no graph changes
    processPendingPrefixUpdates();
  }
}

// periodically submit counters to Counters thread
void
DecisionOld::submitCounters() {
  VLOG(3) << "Submitting counters...";

  // Prepare for submitting counters
  auto counters = spfSolver_->getCounters();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

void
DecisionOld::logRouteEvent(const std::string& event, const int numOfRoutes) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "DecisionOld");
  sample.addString("node_name", myNodeName_);
  sample.addInt("num_of_routes", numOfRoutes);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

void
DecisionOld::logDebounceEvent(
    const int numUpdates, const std::chrono::milliseconds debounceTime) {
  fbzmq::LogSample sample{};

  sample.addString("event", "DECISION_DEBOUNCE");
  sample.addString("entity", "DecisionOld");
  sample.addString("node_name", myNodeName_);
  sample.addInt("updates", numUpdates);
  sample.addInt("duration_ms", debounceTime.count());

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

void
DecisionOld::processPendingUpdates() {
  if (processUpdatesStatus_.adjChanged) {
    processPendingAdjUpdates();
  } else if (processUpdatesStatus_.prefixesChanged) {
    processPendingPrefixUpdates();
  }

  // reset update status
  processUpdatesStatus_.adjChanged = false;
  processUpdatesStatus_.prefixesChanged = false;

  // update decision debounce flag
  processUpdatesBackoff_.reportSuccess();
}

void
DecisionOld::processPendingAdjUpdates() {
  VLOG(1) << "DecisionOld: processing " << pendingAdjUpdates_.getCount()
          << " accumulated adjacency updates.";

  if (!pendingAdjUpdates_.getCount()) {
    LOG(ERROR) << "DecisionOld route computation triggered without any pending "
               << "adjacency updates.";
    return;
  }

  // Retrieve perf events, add debounce perf event, log information to
  // ZmqMonitor, ad and clear pending updates
  auto maybePerfEvents = pendingAdjUpdates_.getPerfEvents();
  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "DECISION_DEBOUNCE");
    auto const& events = maybePerfEvents->events;
    auto const& eventsCnt = events.size();
    CHECK_LE(2, eventsCnt);
    auto duration = events[eventsCnt - 1].unixTs - events[eventsCnt - 2].unixTs;
    logDebounceEvent(
        pendingAdjUpdates_.getCount(), std::chrono::milliseconds(duration));
  }
  pendingAdjUpdates_.clear();

  // run SPF once for all updates received
  LOG(INFO) << "DecisionOld: computing new paths.";
  auto routeDb = spfSolver_->buildMultiPaths(myNodeName_);
  logRouteEvent("ROUTE_CALC", routeDb.routes.size());
  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "DECISION_SPF");
  }
  routeDb.perfEvents = maybePerfEvents;

  // publish the new route state
  auto sendRc = decisionPub_.sendThriftObj(routeDb, serializer_);
  if (sendRc.hasError()) {
    LOG(ERROR) << "Error publishing new routing table: " << sendRc.error();
  }
}

void
DecisionOld::processPendingPrefixUpdates() {
  auto maybePerfEvents = pendingPrefixUpdates_.getPerfEvents();
  pendingPrefixUpdates_.clear();

  // update routeDb once for all updates received
  LOG(INFO) << "DecisionOld: updating new routeDb.";
  auto routeDb = spfSolver_->buildRouteDb(myNodeName_);
  logRouteEvent("ROUTE_CALC", routeDb.routes.size());
  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "ROUTE_UPDATE");
  }
  routeDb.perfEvents = maybePerfEvents;

  // publish the new route state
  auto sendRc = decisionPub_.sendThriftObj(routeDb, serializer_);
  if (sendRc.hasError()) {
    LOG(ERROR) << "Error publishing new routing table: " << sendRc.error();
  }
}

} // namespace openr
