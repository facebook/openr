/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <string>

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

// Default HWM is 1k. We set it to 0 to buffer all received messages.
const int kStoreSubReceiveHwm{0};

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

  vector<thrift::Path> paths;
  for (auto const& kv : adjacencies) {
    auto const& adjacency = kv.first;
    paths.emplace_back(
        FRAGILE,
        addr.isV4() ? adjacency.nextHopV4 : adjacency.nextHopV6,
        adjacency.ifName,
        kv.second /* metric */);
  }

  return thrift::Route(FRAGILE, prefix, std::move(paths));
}

/**
 * Given a thrift::IpPrefix returns a node name for it.
 */
inline std::string
getPrefixVertexName(const thrift::IpPrefix& ipPrefix) {
  return "pfxnd-" + toString(ipPrefix);
}

} // anonymous namespace

/**
 * Private implementation of the SpfSolver
 */
class SpfSolver::SpfSolverImpl {
 public:
  explicit SpfSolverImpl(bool enableV4) : enableV4_(enableV4) {}

  ~SpfSolverImpl() = default;

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

  /**
   * Graph is prepared as followed
   * 1. Add all node vertices (adjacencyDbs_.keys())
   * 2. Use adjacencyDbs_ to add edges between nodes. bidirectional check is
   *    enforced here.
   * 3. Add a vertex for each prefix
   * 4. Add a directed edge from a node-vertex to prefix-vertex if node has
   *    advertised the prefix.
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
  SpfSolverImpl(SpfSolverImpl const&) = delete;
  SpfSolverImpl& operator=(SpfSolverImpl const&) = delete;

  // run SPF and produce map from node name to next-hop and metric to reach it
  unordered_map<
      string /* other node name */,
      pair<string /* next-hop node name */, Weight /* metric */>>
  runSpf(const std::string& myNodeName);

  // adjacencies per advertising router
  std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
      adjacencyDbs_;

  // prefix to set of node names who advertised the prefix
  std::unordered_map<thrift::IpPrefix, std::unordered_set<std::string>>
      prefixToNodeNames_;

  // the graph we operate on for SPF
  Graph graph_;

  // helpers for mapping router names to descriptros and vice versa
  unordered_map<string /* router name */, VertexDescriptor> nameToDescriptor_;
  unordered_map<VertexDescriptor, string /* router name */> descriptorToName_;

  // compare two adjacencies
  struct AdjComp {
    bool
    operator()(
        const thrift::Adjacency& lhs, const thrift::Adjacency& rhs) const {
      return lhs.metric < rhs.metric;
    }
  };

  // Adjacencies indexed by originating router and destination router
  // Note: use multiset, not set, since there are duplicates, i.e., adjacencies
  // with same metric
  unordered_map<
      pair<string /* thisRouterName */, string /* otherRouterName */>,
      multiset<
          thrift::Adjacency,
          AdjComp> /* adjacencies ordered by non-descreasing metric */>
      adjacencyIndex_;

  // track some stats
  fbzmq::ThreadData tData_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};
};

bool
SpfSolver::SpfSolverImpl::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& adjacencyDb) {
  auto const& nodeName = adjacencyDb.thisNodeName;
  VLOG(2) << "Updating adjacency database for node " << nodeName;
  tData_.addStatValue("decision.adj_db_update", 1, fbzmq::COUNT);
  VLOG(3) << "  New adjacencies for node " << nodeName;
  for (auto const& adj : adjacencyDb.adjacencies) {
    VLOG(3) << "  nbr: " << adj.otherNodeName << ", ifName: " << adj.ifName
            << ", metric: " << adj.metric
            << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt;
  }

  // Insert if not exists
  auto res = adjacencyDbs_.emplace(nodeName, adjacencyDb);

  // NOTE: We need to clear SPF cache on any change in Adjacency DB
  // Insertion took place
  if (res.second) {
    return true;
  }

  // There already exists a key. See if value has changed or not
  auto& it = res.first;
  if (adjacencyDb != it->second) {
    it->second = adjacencyDb;
    return true;
  }

  // There is no change
  return false;
}

bool
SpfSolver::SpfSolverImpl::deleteAdjacencyDatabase(const std::string& nodeName) {
  if (adjacencyDbs_.erase(nodeName) > 0) {
    LOG(INFO) << nodeName << "'s adjacency db is deleted";
    return true;
  } else {
    LOG(WARNING) << "Trying to delete adjacency db for nonexisting node "
                 << nodeName;
    return false;
  }
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
SpfSolver::SpfSolverImpl::getAdjacencyDatabases() {
  return adjacencyDbs_;
}

bool
SpfSolver::SpfSolverImpl::updatePrefixDatabase(
    thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;
  VLOG(2) << "Updating prefix database for node " << nodeName;
  tData_.addStatValue("decision.prefix_db_update", 1, fbzmq::COUNT);

  // Add new ones
  bool updated = false;
  std::unordered_set<thrift::IpPrefix> prefixes;
  for (auto const& prefixEntry : prefixDb.prefixEntries) {
    prefixes.insert(prefixEntry.prefix);
    auto& nodes = prefixToNodeNames_[prefixEntry.prefix];
    updated |= nodes.insert(nodeName).second;
  }

  // Remove old ones
  for (auto it = prefixToNodeNames_.begin(); it != prefixToNodeNames_.end();) {
    const auto& prefix = it->first;
    auto& nodes = it->second;
    if (prefixes.count(prefix) or !nodes.count(nodeName)) {
      // a node advertised this prefix before, but not anymore
      ++it;
      continue;
    }

    updated = true;
    nodes.erase(nodeName);
    if (nodes.empty()) {
      it = prefixToNodeNames_.erase(it);
    } else {
      ++it;
    }
  }

  return updated;
}

bool
SpfSolver::SpfSolverImpl::deletePrefixDatabase(const std::string& nodeName) {
  bool updated = false;

  // Renounce all prefixes it has advertised
  for (auto it = prefixToNodeNames_.begin(); it != prefixToNodeNames_.end();) {
    const auto& prefix = it->first;
    auto& nodes = it->second;

    if (nodes.erase(nodeName) == 0) {
      ++it;
      continue;
    }

    LOG(INFO) << nodeName << " no longer advertises " << toString(prefix);
    updated = true;
    if (nodes.empty()) {
      it = prefixToNodeNames_.erase(it);
    } else {
      ++it;
    }
  }

  if (not updated) {
    LOG(WARNING) << "Trying to delete prefix db for nonexisting node "
                 << nodeName;
  }
  return updated;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolver::SpfSolverImpl::getPrefixDatabases() {
  // prefixes per advertising router
  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
      prefixDbs;

  // construct prefix dbs from prefixToNodeNames_
  for (const auto& prefixNodeNames : prefixToNodeNames_) {
    const auto& prefix = prefixNodeNames.first;
    const auto& nodeNames = prefixNodeNames.second;

    for (const auto& nodeName : nodeNames) {
      prefixDbs[nodeName].thisNodeName = nodeName;
      prefixDbs[nodeName].prefixEntries.emplace_back(
          apache::thrift::FRAGILE, prefix, thrift::PrefixType::LOOPBACK, "");
    }
  }

  return prefixDbs;
}

void
SpfSolver::SpfSolverImpl::prepareGraph() {
  VLOG(2) << "SpfSolver::SpfSolverImpl::prepareGraph() called.";

  graph_.clear();
  nameToDescriptor_.clear();
  descriptorToName_.clear();
  adjacencyIndex_.clear();

  // Create a set of all unidirectional adjacencies, which is from one node to
  // another one connected by at least one interface
  std::unordered_set<
      std::pair<std::string /* myNodeName */, std::string /* otherNodeName */>>
      presentAdjacencies;
  for (auto const& kv : adjacencyDbs_) {
    auto const& thisNodeName = kv.first;

    for (auto const& adjacency : kv.second.adjacencies) {
      // Skip adjacency from SPF graph if it is overloaded to avoid transit
      // traffic through it.
      if (adjacency.isOverloaded) {
        continue;
      }

      presentAdjacencies.insert({thisNodeName, adjacency.otherNodeName});
    }
  }

  // Build adjacencyIndex_ with bidirectional check enforced
  for (auto const& kv : adjacencyDbs_) {
    auto const& thisNodeName = kv.first;

    // walk all my ajdacencies
    for (auto const& myAdjacency : kv.second.adjacencies) {
      auto const& otherNodeName = myAdjacency.otherNodeName;

      // Skip if overloaded
      if (myAdjacency.isOverloaded) {
        continue;
      }

      // Check for reverse adjacency
      bool isBidir = presentAdjacencies.count({otherNodeName, thisNodeName});
      if (!isBidir) {
        VLOG(2) << "Failed to find matching adjacency for `" << thisNodeName
                << "' in `" << otherNodeName << "' database";
        continue;
      }

      adjacencyIndex_[{thisNodeName, otherNodeName}].insert(myAdjacency);
    } // for adjacencies
  } // for adjacencyDbs_

  // 1. Add all node vertices
  for (auto const& kv : adjacencyDbs_) {
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
    if (adjacencyDbs_.at(thisNodeName).isOverloaded) {
      metric += Constants::kOverloadNodeMetric;
    }

    // for two nodes with multiple adjacencies in between, use the min metric
    // as their edge metric
    boost::add_edge(srcVertex, dstVertex, EdgeProperty(metric), graph_);
  } // for adjacencyIndex_

  // 3./4. Add all prefix vertices and directed edges from node->prefix vertices
  for (const auto& kv : prefixToNodeNames_) {
    std::string prefixVertexName = getPrefixVertexName(kv.first);

    // Add the prefix vertex
    auto const& prefixVertex = boost::add_vertex(graph_);
    nameToDescriptor_[prefixVertexName] = prefixVertex;
    descriptorToName_[prefixVertex] = prefixVertexName;

    for (const auto& nodeName : kv.second) {
      // Skip the node if we haven't seen it's adjacency DB
      if (!adjacencyDbs_.count(nodeName)) {
        continue;
      }

      // Add edge from node->prefix with zero cost
      auto const& nodeVertex = nameToDescriptor_.at(nodeName);
      boost::add_edge(nodeVertex, prefixVertex, 0 /* cost */, graph_);
    }
  }
} // prepareGraph

/**
 * Compute shortest-path routes from perspective of myNodeName; NOTE: you
 * need to call prepareGraph() to initialize the graph structure before
 * calling this method.
 */
unordered_map<
    string /* otherVertexName (prefix/node) */,
    pair<string /* nextHopVertexName (node) */, Weight>>
SpfSolver::SpfSolverImpl::runSpf(const std::string& myNodeName) {
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
SpfSolver::SpfSolverImpl::buildShortestPaths(const std::string& myNodeName) {
  VLOG(4) << "SpfSolver::buildShortestPaths for " << myNodeName;
  tData_.addStatValue("decision.paths_build_requests", 1, fbzmq::COUNT);

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  if (adjacencyDbs_.count(myNodeName) == 0) {
    LOG(ERROR) << "Could not find adjacency Db for myself: `" << myNodeName
               << "`, skipping spf run";
    return routeDb;
  }

  const auto startTime = std::chrono::steady_clock::now();

  prepareGraph();
  auto spfMap = runSpf(myNodeName);
  const bool myselfOverloaded = adjacencyDbs_.at(myNodeName).isOverloaded;

  for (auto const& kv : prefixToNodeNames_) {
    auto const& prefix = kv.first;
    auto const& nodeNames = kv.second;
    auto const prefixVertexName = getPrefixVertexName(prefix);

    // Skip a prefix if we own it
    if (nodeNames.count(myNodeName)) {
      continue;
    }

    // Find minimum nexthop to reach that prefix
    auto prefixIt = spfMap.find(prefixVertexName);
    if (prefixIt == spfMap.end()) {
      VLOG(2) << "Skipping unreachable prefix " << toString(prefix);
      continue;
    }

    auto const& nextHopNodeName = prefixIt->second.first;
    auto metric = prefixIt->second.second;

    // Subtract overload metric value if myself is overloaded
    if (myselfOverloaded) {
      CHECK_LT(Constants::kOverloadNodeMetric, metric);
      metric -= Constants::kOverloadNodeMetric;
    }

    // Skip route if it is going through a node which is overloaded
    if (metric > Constants::kOverloadNodeMetric) {
      VLOG(2) << "Skipping shortest route to prefix " << toString(prefix)
              << " because it is via an overloaded node.";
      continue;
    }

    // Add a route for this prefix via the nextHopNodeName
    auto const& nhAdjs = adjacencyIndex_.at({myNodeName, nextHopNodeName});
    auto route = createRoute(prefix, *nhAdjs.begin(), metric, enableV4_);
    if (route) {
      routeDb.routes.emplace_back(std::move(*route));
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildShortestPaths took " << deltaTime.count()
            << "ms.";
  tData_.addStatValue(
      "decision.spf.shortestpath_ms", deltaTime.count(), fbzmq::AVG);

  return routeDb;
} // buildShortestPaths

thrift::RouteDatabase
SpfSolver::SpfSolverImpl::buildMultiPaths(const std::string& myNodeName) {
  VLOG(4) << "SpfSolver::buildMultiPaths for " << myNodeName;
  tData_.addStatValue("decision.paths_build_requests", 1, fbzmq::COUNT);

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  if (adjacencyDbs_.count(myNodeName) == 0) {
    LOG(ERROR) << "Could not find adjacency Db for myself: " << myNodeName
               << ", skipping multi-spf run";
    return routeDb;
  }

  const auto startTime = std::chrono::steady_clock::now();

  // This accumulates all direct next-hops toward a given destination prefix.
  // we add those as we find more loop-free alternate neighbors
  std::unordered_map<
      std::string /* prefixVertexName */,
      std::map<
          std::string /* nextHopNodeName */,
          Weight /* metric from nextHopNodeName to prefixVertexName */>>
      allNextHopsForPrefix;

  prepareGraph();
  auto mySpfPaths = runSpf(myNodeName);

  // avoid duplicate iterations over a neighbor which can happen due to multiple
  // adjacencies to it
  std::unordered_set<std::string /* adjacent node name */> visitedAdjNodes;

  // Go over all neighbors and find shortest distance of prefixes from them
  for (auto const& adj : adjacencyDbs_.at(myNodeName).adjacencies) {
    auto const& adjNodeName = adj.otherNodeName;

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

    // Run SPF for the node
    VLOG(4) << "Running SPF for neighbor: " << adjNodeName;
    auto nbrSpfPaths = runSpf(adjNodeName);

    // Walk through all prefixes and find shortest distance of prefix from
    // myNodeName to the prefix via adjNodeName
    for (auto const& kv : prefixToNodeNames_) {
      auto const& prefix = kv.first;
      auto const prefixVertexName = getPrefixVertexName(prefix);

      // Skip if prefix is unreachable from neighbor
      auto prefixIt = nbrSpfPaths.find(prefixVertexName);
      if (prefixIt == nbrSpfPaths.end()) {
        continue; // Prefix is unreachable from neighbor
      }

      // Get distances from adjNodeName
      auto adjPrefixDist = prefixIt->second.second;
      auto adjMyNodeDist = nbrSpfPaths.at(myNodeName).second;

      // Get optimal distance of prefixVertexName from us. Prefix must be
      // reachable from us because it is reachable from our neighbor
      auto myPrefixDist = mySpfPaths.at(prefixVertexName).second;

      // A neighbor (N) can provide loop free alternate to destination (D) from
      // source (S) if and only if
      //   Distance_opt(N, D) < Distance_opt(N, S) + Distance_opt(S, D)
      // For more info read: https://tools.ietf.org/html/rfc5286
      // NOTE: We are using negated condition here to skip the neighbor
      if (adjPrefixDist >= adjMyNodeDist + myPrefixDist) {
        VLOG(4) << "Skipping non LFA nexthop " << adjNodeName << " to "
                << "prefix " << prefixVertexName;
        continue;
      }

      VLOG(4) << "Adding LFA nexthop " << adjNodeName << " for prefix "
              << prefixVertexName;
      allNextHopsForPrefix[prefixVertexName][adjNodeName] = adjPrefixDist;
    } // for prefixToNodeNames_
  } // for adjacencyDbs_.at(myNodeName).adjacencies

  // Build RouteDb for all prefixes including LFAs
  for (auto const& kv : prefixToNodeNames_) {
    auto const& prefix = kv.first;
    auto const& nodeNames = kv.second;
    auto const prefixVertexName = getPrefixVertexName(prefix);

    // Skip a prefix if we own it
    if (nodeNames.count(myNodeName)) {
      continue;
    }

    auto prefixIt = allNextHopsForPrefix.find(prefixVertexName);
    if (prefixIt == allNextHopsForPrefix.end()) {
      VLOG(4) << "Skipping route to unreachable prefix " << prefixVertexName;
      continue;
    }

    std::vector<std::pair<thrift::Adjacency, Weight>> adjacencies;
    for (auto const& nhMetric : prefixIt->second) {
      auto const& nhNodeName = nhMetric.first;
      auto metric = nhMetric.second;

      // Skip route if it is going through a node which is overloaded
      // NOTE: the metric here is a distance to prefix-node via our neighbor
      if (metric > Constants::kOverloadNodeMetric) {
        VLOG(2) << "Skipping shortest route to prefix " << toString(prefix)
                << " because it is via an overloaded node.";
        continue;
      }

      // Add all adjacencies to nextHopNodeName as potential nexthops. FIB will
      // only program ones with min metric and others will act as LFAs
      for (const auto& adj : adjacencyIndex_.at({myNodeName, nhNodeName})) {
        adjacencies.push_back({adj, metric + adj.metric});
      }
    }

    auto route = createRouteMulti(prefix, adjacencies, enableV4_);
    if (route) {
      routeDb.routes.emplace_back(std::move(*route));
    }
  } // for prefixToNodeNames_ (build RouteDb)

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildMultiPaths took " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.spf.multipath_ms", deltaTime.count(), fbzmq::AVG);

  return routeDb;
} // buildMultiPaths

std::unordered_map<std::string, int64_t>
SpfSolver::SpfSolverImpl::getCounters() {
  return tData_.getCounters();
}

//
// Public SpfSolver
//

SpfSolver::SpfSolver(bool enableV4)
    : impl_(new SpfSolver::SpfSolverImpl(enableV4)) {}

SpfSolver::~SpfSolver() {}

// update adjacencies for the given router; everything is replaced
bool
SpfSolver::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& adjacencyDb) {
  return impl_->updateAdjacencyDatabase(adjacencyDb);
}

bool
SpfSolver::deleteAdjacencyDatabase(const std::string& nodeName) {
  return impl_->deleteAdjacencyDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
SpfSolver::getAdjacencyDatabases() {
  return impl_->getAdjacencyDatabases();
}

// update prefixes for a given router
bool
SpfSolver::updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb) {
  return impl_->updatePrefixDatabase(prefixDb);
}

bool
SpfSolver::deletePrefixDatabase(const std::string& nodeName) {
  return impl_->deletePrefixDatabase(nodeName);
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolver::getPrefixDatabases() {
  return impl_->getPrefixDatabases();
}

thrift::RouteDatabase
SpfSolver::buildShortestPaths(const std::string& myNodeName) {
  return impl_->buildShortestPaths(myNodeName);
}

thrift::RouteDatabase
SpfSolver::buildMultiPaths(const std::string& myNodeName) {
  return impl_->buildMultiPaths(myNodeName);
}

std::unordered_map<std::string, int64_t>
SpfSolver::getCounters() {
  return impl_->getCounters();
}

//
// Decision class implementation
//

Decision::Decision(
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
    : expBackoff_(debounceMinDur, debounceMaxDur),
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
  processPendingUpdatesTimer_ = fbzmq::ZmqTimeout::make(
      this, [this]() noexcept { processPendingUpdates(); });
  spfSolver_ = std::make_unique<SpfSolver>(enableV4);

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Initialize ZMQ sockets
  scheduleTimeout(
      std::chrono::seconds(0), [this, &zmqContext]() { prepare(zmqContext); });
}

void
Decision::prepare(fbzmq::Context& zmqContext) noexcept {
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

  VLOG(2) << "Decision thread attaching socket/event callbacks...";

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);

  // Attach callback for processing publications on storeSub_ socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*storeSub_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "Decision: publication received...";

        auto maybeThriftPub = storeSub_.recvThriftObj<thrift::Publication>(
            serializer_);
        if (maybeThriftPub.hasError()) {
          LOG(ERROR) << "Error processing KvStore publication: "
                     << maybeThriftPub.error();
          return;
        }

        // Apply publication and compute routes with exponential
        // backoff timer if needed
        if (processPublication(maybeThriftPub.value())) {
          if (!expBackoff_.atMaxBackoff()) {
            expBackoff_.reportError();
            processPendingUpdatesTimer_->scheduleTimeout(
                expBackoff_.getTimeRemainingUntilRetry());
          } else {
            CHECK(processPendingUpdatesTimer_->isScheduled());
          }
        }
      });

  // Attach callback for processing requests on REP socket
  addSocket(
      fbzmq::RawZmqSocketPtr{*decisionRep_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(3) << "Decision: request received...";
        processRequest();
      });

  initialSync(zmqContext);
}

void
Decision::processRequest() {
  auto maybeThriftReq = decisionRep_.recvThriftObj<thrift::DecisionRequest>(
      serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "Decision: Error processing request on REP socket: "
               << maybeThriftReq.error();
    return;
  }

  auto thriftReq = maybeThriftReq.value();
  thrift::DecisionReply reply;
  switch (thriftReq.cmd) {
  case thrift::DecisionCommand::ROUTE_DB_GET: {
    auto nodeName = thriftReq.nodeName;
    if (nodeName.empty()) {
      VLOG(1) << "Decision: Routes requested with no specific node name. "
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
Decision::getCounters() {
  return spfSolver_->getCounters();
}

bool
Decision::processPublication(thrift::Publication const& thriftPub) {
  bool graphChanged{false};

  // LSDB addition/update
  // deserialize contents of every LSDB key

  // Nothing to process if no adj/prefix db changes
  if (thriftPub.keyVals.empty() and thriftPub.expiredKeys.empty()) {
    return false;
  }

  for (const auto& kv : thriftPub.keyVals) {
    const auto& key = kv.first;
    const auto& rawVal = kv.second;
    std::string prefix, nodeName;
    folly::split(Constants::kPrefixNameSeparator, key, prefix, nodeName);

    if (not rawVal.value.hasValue()) {
      // skip TTL update
      DCHECK(rawVal.ttlVersion > 0);
      continue;
    }

    try {
      if (key.find(adjacencyDbMarker_) == 0) {
        auto adjacencyDb =
            fbzmq::util::readThriftObjStr<thrift::AdjacencyDatabase>(
                rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, adjacencyDb.thisNodeName);
        if (spfSolver_->updateAdjacencyDatabase(adjacencyDb)) {
          graphChanged = true;
          pendingUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
        }
        continue;
      }

      if (key.find(prefixDbMarker_) == 0) {
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            rawVal.value.value(), serializer_);
        CHECK_EQ(nodeName, prefixDb.thisNodeName);
        if (spfSolver_->updatePrefixDatabase(prefixDb)) {
          graphChanged = true;
          pendingUpdates_.addUpdate(myNodeName_, prefixDb.perfEvents);
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
    folly::split(Constants::kPrefixNameSeparator, key, prefix, nodeName);

    if (key.find(adjacencyDbMarker_) == 0) {
      if (spfSolver_->deleteAdjacencyDatabase(nodeName)) {
        graphChanged = true;
        pendingUpdates_.addUpdate(myNodeName_, folly::none);
      }
      continue;
    }

    if (key.find(prefixDbMarker_) == 0) {
      if (spfSolver_->deletePrefixDatabase(nodeName)) {
        graphChanged = true;
        pendingUpdates_.addUpdate(myNodeName_, folly::none);
      }
      continue;
    }
  }

  return graphChanged;
}

// perform full dump of all LSDBs and run initial routing computations
void
Decision::initialSync(fbzmq::Context& zmqContext) {
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

  VLOG(2) << "Decision process requesting initial state...";

  // receive the full dump of the database
  auto maybeThriftPub = storeReq.recvThriftObj<thrift::Publication>(
      serializer_, Constants::kReadTimeout);
  if (maybeThriftPub.hasError()) {
    LOG(ERROR) << "Error processing KvStore publication: "
               << maybeThriftPub.error();
    return;
  }

  // Process publication and immediately apply updates
  if (processPublication(maybeThriftPub.value())) {
    processPendingUpdates();
  }
}

// periodically submit counters to Counters thread
void
Decision::submitCounters() {
  VLOG(2) << "Submitting counters...";

  // Prepare for submitting counters
  auto counter = spfSolver_->getCounters();
  counter["decision.aliveness"] = 1;
  fbzmq::CounterMap submittingCounters = prepareSubmitCounters(counter);

  zmqMonitorClient_->setCounters(submittingCounters);
}

void
Decision::logRouteEvent(const std::string& event, const int numOfRoutes) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "Decision");
  sample.addString("node_name", myNodeName_);
  sample.addInt("num_of_routes", numOfRoutes);

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

void
Decision::logDebounceEvent(
    const int numUpdates, const std::chrono::milliseconds debounceTime) {
  fbzmq::LogSample sample{};

  sample.addString("event", "DECISION_DEBOUNCE");
  sample.addString("entity", "Decision");
  sample.addString("node_name", myNodeName_);
  sample.addInt("updates", numUpdates);
  sample.addInt("duration_ms", debounceTime.count());

  zmqMonitorClient_->addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

void
Decision::processPendingUpdates() {
  VLOG(1) << "Decision: processing " << pendingUpdates_.getCount()
          << " accumulated updates.";

  if (!pendingUpdates_.getCount()) {
    LOG(ERROR) << "Decision route computation triggered without any pending "
               << "updates.";
    return;
  }

  // Retrieve perf events, add debounce perf event, log information to
  // ZmqMonitor, ad and clear pending updates
  auto maybePerfEvents = pendingUpdates_.getPerfEvents();
  if (maybePerfEvents) {
    addPerfEvent(*maybePerfEvents, myNodeName_, "DECISION_DEBOUNCE");
    auto const& events = maybePerfEvents->events;
    auto const& eventsCnt = events.size();
    CHECK_LE(2, eventsCnt);
    auto duration = events[eventsCnt - 1].unixTs - events[eventsCnt - 2].unixTs;
    logDebounceEvent(
        pendingUpdates_.getCount(), std::chrono::milliseconds(duration));
  }
  pendingUpdates_.clear();

  // run SPF once for all updates received
  LOG(INFO) << "Decision: computing new paths.";
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

  // update decision debounce flag
  expBackoff_.reportSuccess();
}

} // namespace openr
