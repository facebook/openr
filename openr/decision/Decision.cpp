/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Decision.h"

#include <chrono>
#include <set>
#include <string>
#include <unordered_set>

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

using Metric = uint64_t;

namespace {

// Default HWM is 1k. We set it to 0 to buffer all received messages.
const int kStoreSubReceiveHwm{0};

//
// Why define Link and LinkState? Isn't link state fully captured by something
// like std::unordered_map<std::string, thrift::AdjacencyDatabase>?
// It is, but these classes provide a few major benefits over that simple
// structure:
//
// 1. Only stores bidirectional links. i.e. for a link, the node at both ends
// is advertising the adjancecy
//
// 2. Defines a hash and comparators that operate on the essential property of a
// network link: the tuple: unorderedPair<orderedPair<nodeName, ifName>,
//                                orderedPair<nodeName, ifName>>
//
// 3. For each unique link in the network, holds a single object that can be
// quickly accessed and modified via the nodeName of either end of the link.
//
// 4. Provides useful apis to read and write link state.
//

class Link {
 public:
  Link(const std::string& nodeName1, const openr::thrift::Adjacency& adj1,
       const std::string& nodeName2, const openr::thrift::Adjacency& adj2)
       : n1_(nodeName1), n2_(nodeName2),
         if1_(adj1.ifName), if2_(adj2.ifName),
         metric1_(adj1.metric), metric2_(adj2.metric),
         overload1_(adj1.isOverloaded), overload2_(adj2.isOverloaded),
         nhV41_(adj1.nextHopV4), nhV42_(adj2.nextHopV4),
         nhV61_(adj1.nextHopV6), nhV62_(adj2.nextHopV6),
         orderedNames(std::minmax(std::make_pair(n1_, if1_),
                                  std::make_pair(n2_, if2_))),
         hash(std::hash<
           std::pair<std::pair<std::string, std::string>,
                     std::pair<std::string, std::string>>>()(orderedNames)) {}

 private:
  const std::string n1_, n2_, if1_, if2_;
  Metric metric1_{1}, metric2_{1};
  bool overload1_{false}, overload2_{false};
  openr::thrift::BinaryAddress nhV41_, nhV42_, nhV61_, nhV62_;
  const std::pair<
    std::pair<std::string, std::string>,
    std::pair<std::string, std::string>> orderedNames;

 public:
  const size_t hash{0};

  const std::string& getOtherNodeName(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return n2_;
    }
    if (n2_ == nodeName) {
      return n1_;
    }
    throw std::invalid_argument(nodeName);
  }

  const std::string& firstNodeName() const {
    return orderedNames.first.first;
  }

  const std::string& secondNodeName() const {
    return orderedNames.second.first;
  }

  const std::string& getIfaceFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return if1_;
    }
    if (n2_ == nodeName) {
      return if2_;
    }
    throw std::invalid_argument(nodeName);
  }

  Metric getMetricFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return metric1_;
    }
    if (n2_ == nodeName) {
      return metric2_;
    }
    throw std::invalid_argument(nodeName);
  }

  bool getOverloadFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return overload1_;
    }
    if (n2_ == nodeName) {
      return overload2_;
    }
    throw std::invalid_argument(nodeName);
  }

  bool isOverloaded() const {
    return overload1_ || overload2_;
  }

  const openr::thrift::BinaryAddress&
  getNhV4FromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return nhV41_;
    }
    if (n2_ == nodeName) {
      return nhV42_;
    }
    throw std::invalid_argument(nodeName);
  }

  const openr::thrift::BinaryAddress&
  getNhV6FromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return nhV61_;
    }
    if (n2_ == nodeName) {
      return nhV62_;
    }
    throw std::invalid_argument(nodeName);
  }

  void setNhV4FromNode(const std::string& nodeName,
      const openr::thrift::BinaryAddress& nhV4) {
    if (n1_ == nodeName) {
      nhV41_ = nhV4;
    } else if (n2_ == nodeName) {
      nhV42_ = nhV4;
    } else {
      throw std::invalid_argument(nodeName);
    }
  }

  void setNhV6FromNode(const std::string& nodeName,
      const openr::thrift::BinaryAddress& nhV6) {
    if (n1_ == nodeName) {
      nhV61_ = nhV6;
    } else if (n2_ == nodeName) {
      nhV62_ = nhV6;
    } else {
      throw std::invalid_argument(nodeName);
    }
  }

  void setMetricFromNode(const std::string& nodeName, Metric d) {
    if (n1_ == nodeName) {
      metric1_ = d;
    } else if (n2_ == nodeName) {
      metric2_ = d;
    } else {
      throw std::invalid_argument(nodeName);
    }
  }

  void setOverloadFromNode(const std::string& nodeName, bool overload) {
    if (n1_ == nodeName) {
      overload1_ = overload;
    } else if (n2_ == nodeName) {
      overload2_ = overload;
    } else {
      throw std::invalid_argument(nodeName);
    }
  }

  bool operator<(const Link& other) const {
    return this->hash < other.hash;
  }

  bool operator==(const Link& other) const {
    if (this->hash != other.hash) {
      return false;
    }
    return this->orderedNames == other.orderedNames;
  }

  std::string toString() const {
    return folly::sformat("{}%{} <---> {}%{}", n1_, if1_, n2_, if2_);
  }

  std::string directionalToString(const std::string& fromNode) const {
    return folly::sformat("{}%{} ---> {}%{}",
        fromNode,
        getIfaceFromNode(fromNode),
        getOtherNodeName(fromNode),
        getIfaceFromNode(getOtherNodeName(fromNode)));
  }
}; // class Link

// Classes needed for running Dijkstra
class DijkstraQNode {
 public:
  DijkstraQNode(const std::string& n, Metric d):
     nodeName(n), distance(d) {}
  const std::string nodeName;
  Metric distance{0};
  std::unordered_set<std::string> nextHops;
};

class DijkstraQ {
 private:
  std::vector<std::shared_ptr<DijkstraQNode>> heap_;
  std::unordered_map<std::string, std::shared_ptr<DijkstraQNode>> nameToNode_;

  struct {
    bool operator()(std::shared_ptr<DijkstraQNode> a,
        std::shared_ptr<DijkstraQNode> b) const {
      if (a->distance != b->distance) {
        return a->distance > b->distance;
      }
      return a->nodeName > b->nodeName;
    }
  } DijkstraQNodeGreater;

 public:

  void insertNode(const std::string& nodeName, Metric d) {
    heap_.push_back(std::make_shared<DijkstraQNode>(nodeName, d));
    nameToNode_[nodeName] = heap_.back();
    std::push_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
  }

  std::shared_ptr<DijkstraQNode>
  get(const std::string& nodeName) {
    if (nameToNode_.count(nodeName)) {
      return nameToNode_.at(nodeName);
    }
    return nullptr;
  }

  std::shared_ptr<DijkstraQNode> extractMin() {
    if (heap_.empty()) {
      return nullptr;
    }
    auto min = heap_.at(0);
    CHECK(nameToNode_.erase(min->nodeName));
    std::pop_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
    heap_.pop_back();
    return min;
  }

  void decreaseKey(const std::string& nodeName, Metric d) {
    if (nameToNode_.count(nodeName)) {
      if (nameToNode_.at(nodeName)->distance < d) {
        throw std::invalid_argument(std::to_string(d));
      }
      nameToNode_.at(nodeName)->distance = d;
      // this is a bit slow but is rarely called in our application. In fact,
      // in networks where the metric is hop count, this will never be called
      // and the Dijkstra run is no different than BFS
      std::make_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
    } else {
      throw std::invalid_argument(nodeName);
    }
  }
};

} // anonymous namespace

namespace std {

// needed for certain containers

template <>
struct hash<Link> {
  size_t operator()(Link const& link) const {
    return link.hash;
  }
};

} // namespace std


namespace {


class LinkState {
 public:

  struct LinkPtrHash {
   bool operator()(const std::shared_ptr<Link>& l) const {
     return l->hash;
   }
  };

  struct LinkPtrLess {
    bool operator()(const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const {
      return *lhs < *rhs;
    }
  };

  struct LinkPtrEqual {
    bool operator()(const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const {
      return *lhs == *rhs;
    }
  };

  // for both these cotainers, we want to compare the actual link being stored
  // and not the object address
  using LinkSet =
      std::unordered_set<std::shared_ptr<Link>, LinkPtrHash, LinkPtrEqual>;
  using OrderedLinkSet = std::set<std::shared_ptr<Link>, LinkPtrLess>;

  void addLink(const Link& link) {
    auto key = std::make_shared<Link>(link);
    CHECK(linkMap_[link.firstNodeName()].insert(key).second);
    CHECK(linkMap_[link.secondNodeName()].insert(key).second);
  }

  // throws std::out_of_range if links are not present
  void removeLink(const Link& link) {
    auto key = std::make_shared<Link>(link);
    CHECK(linkMap_.at(link.firstNodeName()).erase(key));
    CHECK(linkMap_.at(link.secondNodeName()).erase(key));
  }

  void removeLinksFromNode(const std::string& nodeName) {
    // erase ptrs to these links from other nodes
    for (auto const& link : linkMap_.at(nodeName)) {
      CHECK(linkMap_.at(link->getOtherNodeName(nodeName)).erase(link));
    }
    linkMap_.erase(nodeName);
  }

  const LinkSet& linksFromNode(const std::string& nodeName) {
    static const LinkSet defaultEmptySet;
    auto search = linkMap_.find(nodeName);
    if (search != linkMap_.end()) {
      return search->second;
    }
    return defaultEmptySet;
  }

  OrderedLinkSet orderedLinksFromNode(const std::string& nodeName) {
    OrderedLinkSet set;
    auto search = linkMap_.find(nodeName);
    if (search != linkMap_.end()) {
      for (auto const& link : search->second) {
        set.emplace(link);
      }
    }
    return set;
  }

 private:
  // this stores the same link object accessible from either nodeName
  std::unordered_map<std::string /* nodeName */, LinkSet> linkMap_;

}; // class LinkState
} // anonymous namespace


namespace openr {

/**
 * Private implementation of the SpfSolver
 */
class SpfSolver::SpfSolverImpl {
 public:
  SpfSolverImpl(
      const std::string& myNodeName,
      bool enableV4,
      bool computeLfaPaths)
      : myNodeName_(myNodeName),
        enableV4_(enableV4),
        computeLfaPaths_(computeLfaPaths) {}

  ~SpfSolverImpl() = default;

  std::pair<bool /* topology has changed*/,
            bool /* local nextHop addrs have changed */>
  updateAdjacencyDatabase(thrift::AdjacencyDatabase const& newAdjacencyDb);

  // returns true if the AdjacencyDatabase existed
  bool deleteAdjacencyDatabase(const std::string& nodeName);

  std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
  getAdjacencyDatabases();
  // returns true if the prefixDb changed
  bool updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb);

  // returns true if the PrefixDatabase existed
  bool deletePrefixDatabase(const std::string& nodeName);

  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases();

  folly::Optional<thrift::RouteDatabase> buildPaths(
      const std::string& myNodeName);
  folly::Optional<thrift::RouteDatabase> buildRouteDb(
      const std::string& myNodeName);

  std::unordered_map<std::string, int64_t> getCounters();

  fbzmq::ThreadData&
  getThreadData() noexcept {
    return tData_;
  }

 private:
  // no copy
  SpfSolverImpl(SpfSolverImpl const&) = delete;
  SpfSolverImpl& operator=(SpfSolverImpl const&) = delete;

  // run SPF and produce map from node name to next-hops that have shortest
  // paths to it
  unordered_map<
      string /* otherNodeName */,
      pair<Metric, unordered_set<string /* nextHopNodeName */>>>
  runSpf(const std::string& nodeName);

  std::set<Link> getOrderedLinkSet(const thrift::AdjacencyDatabase& adjDb);

  Metric findMinDistToNeighbor(
    const std::string& myNodeName, const std::string& neighborName);

  // returns Link object if the reverse adjancency is present in
  // adjacencyDatabases_.at(adj.otherNodeName), else returns folly::none
  folly::Optional<Link> maybeMakeLink(
    const std::string& nodeName, const thrift::Adjacency& adj);

  std::unordered_map<
      std::string, thrift::AdjacencyDatabase> adjacencyDatabases_;

  LinkState linkState_;

  // Save all direct next-hop distance from a given source node to a destination
  // node. We update it as we compute all LFA routes from perspective of source
  std::unordered_map<
      std::string /* source nodeName */,
      unordered_map<
          string /* otherNodeName */,
          pair<Metric, unordered_set<string /* nextHopNodeName */>>>>
      spfResults_;

  // For each prefix in the network, stores a set of nodes that advertise it
  std::unordered_map<
    thrift::IpPrefix, std::unordered_set<std::string>> prefixes_;
  std::unordered_map<std::string, thrift::PrefixDatabase> prefixDatabases_;

  // track some stats
  fbzmq::ThreadData tData_;

  const std::string myNodeName_;

  // is v4 enabled. If yes then Decision will forward v4 prefixes with v4
  // nexthops to Fib module for programming. Else it will just drop them.
  const bool enableV4_{false};

  const bool computeLfaPaths_{false};
};

std::pair<bool /* topology has changed*/,
          bool /* local nextHop addrs have changed */>
SpfSolver::SpfSolverImpl::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb) {
  auto const& nodeName = newAdjacencyDb.thisNodeName;
  VLOG(2) << "Updating adjacency database for node " << nodeName;
  tData_.addStatValue("decision.adj_db_update", 1, fbzmq::COUNT);

  for (auto const& adj : newAdjacencyDb.adjacencies) {
    VLOG(3) << "  neighbor: " << adj.otherNodeName << ", remoteIfName: "
            << getRemoteIfName(adj)
            << ", ifName: " << adj.ifName << ", metric: " << adj.metric
            << ", overloaded: " << adj.isOverloaded << ", rtt: " << adj.rtt;
  }

  // Default construct if it did not exist
  thrift::AdjacencyDatabase priorAdjacencyDb(
      std::move(adjacencyDatabases_[nodeName]));
  // replace
  adjacencyDatabases_[nodeName] = newAdjacencyDb;

  // for comparing old and new state, we order the links based on the tuple
  // <nodeName1, iface1, nodeName2, iface2>, this allows us to easily discern
  // topology changes in the single loop below
  auto oldLinks = linkState_.orderedLinksFromNode(nodeName);
  auto newLinks = getOrderedLinkSet(newAdjacencyDb);

  // fill these sets with the appropriate links
  std::unordered_set<Link> linksUp;
  std::unordered_set<Link> linksDown;

  bool topoChanged =
      newAdjacencyDb.isOverloaded != priorAdjacencyDb.isOverloaded;

  bool localNextHopsChanged = false;

  auto newIter = newLinks.begin();
  auto oldIter = oldLinks.begin();
  while (newIter != newLinks.end() || oldIter != oldLinks.end()) {
    if (newIter != newLinks.end() &&
        (oldIter == oldLinks.end() || *newIter < **oldIter)) {
      // newIter is pointing at a Link not currently present, record this as a
      // link to add and advance newIter
      topoChanged = true;
      linkState_.addLink(*newIter);
      ++newIter;
      continue;
    }
    if (oldIter != oldLinks.end() &&
        (newIter == newLinks.end() || **oldIter < *newIter)) {
      // oldIter is pointing at a Link that is no longer present, record this as
      // a link to remove and advance oldIter
      topoChanged = true;
      linkState_.removeLink(**oldIter);
      ++oldIter;
      continue;
    }
    // The newIter and oldIter point to the same link. This link did not go up
    // or down. The topology may still have changed though if the link overlaod
    // or metric changed
    if (newIter->getMetricFromNode(nodeName) !=
        (*oldIter)->getMetricFromNode(nodeName)) {
      topoChanged = true;
      // change the metric on the link object we already have
      (*oldIter)->setMetricFromNode(
          nodeName, newIter->getMetricFromNode(nodeName));

      VLOG(3) << folly::sformat(
          "Metric change on link {}: {} => {}",
          newIter->directionalToString(nodeName),
          (*oldIter)->getMetricFromNode(nodeName),
          newIter->getMetricFromNode(nodeName));
    }
    if (newIter->getOverloadFromNode(nodeName) !=
        (*oldIter)->getOverloadFromNode(nodeName)) {

      // for spf, we do not consider simplex overloading, so there is no need to
      // rerun unless this is true
      topoChanged |= newIter->isOverloaded() != (*oldIter)->isOverloaded();

      // change the overload value in the link object we already have
      (*oldIter)->setOverloadFromNode(
          nodeName, newIter->getOverloadFromNode(nodeName));
      VLOG(3) << folly::sformat(
          "Overload change on link {}: {} => {}",
          newIter->directionalToString(nodeName),
          (*oldIter)->getOverloadFromNode(nodeName),
          newIter->getOverloadFromNode(nodeName));
    }
    // check if local nextHops Changed
    if(newIter->getNhV4FromNode(nodeName) !=
        (*oldIter)->getNhV4FromNode(nodeName)) {
      localNextHopsChanged = myNodeName_ == nodeName;
      (*oldIter)->setNhV4FromNode(nodeName,
          newIter->getNhV4FromNode(nodeName));
    }
    if(newIter->getNhV6FromNode(nodeName) !=
        (*oldIter)->getNhV6FromNode(nodeName)) {
      localNextHopsChanged = myNodeName_ == nodeName;
      (*oldIter)->setNhV6FromNode(nodeName,
          newIter->getNhV6FromNode(nodeName));
    }
    ++newIter;
    ++oldIter;
  }

  return std::make_pair(topoChanged, localNextHopsChanged);
}


bool
SpfSolver::SpfSolverImpl::deleteAdjacencyDatabase(const std::string& nodeName) {
  auto search = adjacencyDatabases_.find(nodeName);

  if (search == adjacencyDatabases_.end()) {
    LOG(WARNING) << "Trying to delete adjacency db for nonexisting node "
                 << nodeName;
    return false;
  }
  linkState_.removeLinksFromNode(nodeName);
  adjacencyDatabases_.erase(search);
  return true;
}

std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
SpfSolver::SpfSolverImpl::getAdjacencyDatabases() {
  return adjacencyDatabases_;
}

bool
SpfSolver::SpfSolverImpl::updatePrefixDatabase(
    thrift::PrefixDatabase const& prefixDb) {
  auto const& nodeName = prefixDb.thisNodeName;
  VLOG(2) << "Updating prefix database for node " << nodeName;
  tData_.addStatValue("decision.prefix_db_update", 1, fbzmq::COUNT);

  std::set<thrift::IpPrefix> oldPrefixSet;
  for (const auto& prefixEntry : prefixDatabases_[nodeName].prefixEntries) {
    oldPrefixSet.emplace(prefixEntry.prefix);
  }

  // update the entry
  prefixDatabases_[nodeName] = prefixDb;
  std::set<thrift::IpPrefix> newPrefixSet;
  for (const auto& prefixEntry : prefixDb.prefixEntries) {
    newPrefixSet.emplace(prefixEntry.prefix);
  }


  std::set<thrift::IpPrefix> prefixesToRemove;
  std::set_difference(
    oldPrefixSet.begin(), oldPrefixSet.end(),
    newPrefixSet.begin(), newPrefixSet.end(),
    std::inserter(prefixesToRemove, prefixesToRemove.begin()));

  std::set<thrift::IpPrefix> prefixesToAdd;
  std::set_difference(
    newPrefixSet.begin(), newPrefixSet.end(),
    oldPrefixSet.begin(), oldPrefixSet.end(),
    std::inserter(prefixesToAdd, prefixesToAdd.begin()));

  for (const auto& prefix : prefixesToRemove) {
    auto& nodeList = prefixes_.at(prefix);
    nodeList.erase(nodeName);
    if (nodeList.empty()) {
      prefixes_.erase(prefix);
    }
  }
  for (const auto& prefix : prefixesToAdd) {
    prefixes_[prefix].emplace(nodeName);
  }

  return !(prefixesToAdd.empty() && prefixesToRemove.empty());
}

bool
SpfSolver::SpfSolverImpl::deletePrefixDatabase(const std::string& nodeName) {
  auto search = prefixDatabases_.find(nodeName);
  if (search == prefixDatabases_.end() ||
      search->second.prefixEntries.empty()) {
    LOG(INFO) << "Trying to delete empty or non-existent prefix db for node "
              << nodeName;
    if (search != prefixDatabases_.end()) {
      prefixDatabases_.erase(search);
    }
    return false;
  }

  for (const auto& prefixEntry : search->second.prefixEntries) {
    auto& nodeList = prefixes_.at(prefixEntry.prefix);
    nodeList.erase(nodeName);
    if (nodeList.empty()) {
      prefixes_.erase(prefixEntry.prefix);
    }
  }

  prefixDatabases_.erase(search);
  return true;
}

std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
SpfSolver::SpfSolverImpl::getPrefixDatabases() {
  return prefixDatabases_;
}

/**
 * Compute shortest-path routes from perspective of nodeName;
 */
unordered_map<
    string /* otherNodeName */,
    pair<Metric, unordered_set<string /* nextHopNodeName */>>>
SpfSolver::SpfSolverImpl::runSpf(const std::string& thisNodeName) {
  unordered_map<string, pair<Metric, unordered_set<string>>> result;

  tData_.addStatValue("decision.spf_runs", 1, fbzmq::COUNT);
  const auto startTime = std::chrono::steady_clock::now();

  DijkstraQ q;
  q.insertNode(thisNodeName, 0);
  uint64_t loop = 0;
  for (auto node = q.extractMin(); node; node = q.extractMin()) {
    ++loop;
    // we've found this node's shortest paths. record it
    auto emplaceRc = result.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(node->nodeName),
      std::forward_as_tuple(node->distance, std::move(node->nextHops)));
    CHECK(emplaceRc.second);

    auto& recordedNodeName = emplaceRc.first->first;
    auto& recordedNodeMetric = emplaceRc.first->second.first;
    auto& recordedNodeNextHops = emplaceRc.first->second.second;

    if (adjacencyDatabases_.at(recordedNodeName).isOverloaded
        && recordedNodeName != thisNodeName) {
      // no transit traffic through this node. we've recorded the nexthops to
      // this node, but will not consider any of it's adjancecies as offering
      // lower cost paths towards further away nodes. This effectively drains
      // traffic away from this node
      continue;
    }
    // we have the shortest path nexthops for recordedNodeName. Use these
    // nextHops for any node that is connected to recordedNodeName that doesn't
    // already have a lower cost path from thisNodeName
    //
    // this is the "relax" step in the Dijkstra Algorithim pseudocode in CLRS
    for (const auto& link : linkState_.linksFromNode(recordedNodeName)) {
      auto otherNodeName = link->getOtherNodeName(recordedNodeName);
      if (link->isOverloaded() || result.count(otherNodeName)) {
        continue;
      }
      auto metric = link->getMetricFromNode(recordedNodeName);
      auto otherNode = q.get(otherNodeName);
      if (!otherNode) {
        q.insertNode(otherNodeName, recordedNodeMetric + metric);
        otherNode = q.get(otherNodeName);
      }
      if (otherNode->distance >= recordedNodeMetric + metric) {
        // recordedNodeName is either along an alternate shortest path towards
        // otherNodeName or is along a new shorter path. In either case,
        // otherNodeName should use recordedNodeName's nextHops until it finds
        // some shorter path
        if (otherNode->distance > recordedNodeMetric + metric) {
          // if this is strictly better, forget about any other nexthops
          otherNode->nextHops.clear();
          q.decreaseKey(otherNode->nodeName, recordedNodeMetric + metric);
        }
        otherNode->nextHops.insert(
          recordedNodeNextHops.begin(), recordedNodeNextHops.end());
      }
      if (otherNode->nextHops.empty()) {
        // this node is directly connected to the source
        otherNode->nextHops.emplace(otherNode->nodeName);
      }
    }
  }
  VLOG(3) << "Dijkstra loop count: " << loop;
  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "SPF elapsed time: " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.spf_ms", deltaTime.count(), fbzmq::AVG);
  return result;
}

folly::Optional<thrift::RouteDatabase>
SpfSolver::SpfSolverImpl::buildPaths(const std::string& myNodeName) {
  if (adjacencyDatabases_.count(myNodeName) == 0) {
    return folly::none;
  }

  auto const& startTime = std::chrono::steady_clock::now();
  tData_.addStatValue("decision.paths_build_requests", 1, fbzmq::COUNT);

  spfResults_.clear();
  spfResults_[myNodeName] = runSpf(myNodeName);
  if (computeLfaPaths_) {
    // avoid duplicate iterations over a neighbor which can happen due to
    // multiple adjacencies to it
    std::unordered_set<std::string /* adjacent node name */> visitedAdjNodes;
    for (auto const& link : linkState_.linksFromNode(myNodeName)) {
      auto const& otherNodeName = link->getOtherNodeName(myNodeName);
      // Skip if already visited
      if (!visitedAdjNodes.insert(otherNodeName).second ||
          link->isOverloaded()) {
        continue;
      }
      spfResults_[otherNodeName] = runSpf(otherNodeName);
    }
  }

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildPaths took " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.build_paths_ms", deltaTime.count(), fbzmq::AVG);

  return buildRouteDb(myNodeName);
} // buildPaths

folly::Optional<thrift::RouteDatabase>
SpfSolver::SpfSolverImpl::buildRouteDb(const std::string& myNodeName) {
  if (adjacencyDatabases_.count(myNodeName) == 0 ||
      spfResults_.count(myNodeName) == 0) {
    return folly::none;
  }

  const auto startTime = std::chrono::steady_clock::now();
  tData_.addStatValue("decision.route_build_requests", 1, fbzmq::COUNT);
  const auto& shortestPathsFromHere = spfResults_.at(myNodeName);

  thrift::RouteDatabase routeDb;
  routeDb.thisNodeName = myNodeName;
  for (const auto& kv : prefixes_) {
    const auto& prefix = kv.first;
    const auto& nodesWithPrefix = kv.second;
    if (nodesWithPrefix.count(myNodeName)) {
      // skip adding route for prefixes advertised by this node
      continue;
    }
    auto prefixStr = prefix.prefixAddress.addr;
    bool isV4Prefix = prefixStr.size() == folly::IPAddressV4::byteCount();
    if (isV4Prefix && !enableV4_) {
      LOG(WARNING) << "Received v4 prefix while v4 is not enabled.";
      continue;
    }

    Metric prefixMetric = std::numeric_limits<Metric>::max();

    // find the set of the closest nodes that advertise this prefix
    std::unordered_set<std::string> minCostNodes;
    for (const auto& node : nodesWithPrefix) {
      try {
        const auto nodeDistance = shortestPathsFromHere.at(node).first;
        if (prefixMetric >= nodeDistance) {
          if (prefixMetric > nodeDistance) {
            prefixMetric = nodeDistance;
            minCostNodes.clear();
          }
          minCostNodes.emplace(node);
        }
      } catch (std::out_of_range const& e) {
        LOG(WARNING) << "No path to " << node << " from " << myNodeName
                     << " for prefix: " << toString(prefix);
      }
    }

    if (minCostNodes.empty()) {
      LOG(WARNING) << "No route to prefix " << toString(prefix)
                   << ", advertised by: " << folly::join(", ", nodesWithPrefix);
      tData_.addStatValue("decision.no_route_to_prefix", 1, fbzmq::COUNT);
      continue;
    }

    // build up next hop nodes both nodes that are along a shortest path to the
    // prefix and, if enabled, those with an LFA path to the prefix
    std::unordered_map<
      std::string /* nextHopNodeName */,
      Metric /* the distance from the nexthop to the dest */> nextHopNodes;

    // add neighbors with shortest path to the prefix
    for (const auto& node : minCostNodes) {
      for (const auto& nhName : shortestPathsFromHere.at(node).second) {
        nextHopNodes[nhName] =
            prefixMetric - findMinDistToNeighbor(myNodeName, nhName);
      }
    }

    // add any other neighbors that have LFA paths to the prefix
    if (computeLfaPaths_) {
      for (const auto& kv2 : spfResults_) {
        const auto& neighborName = kv2.first;
        const auto& shortestPathsFromNeighbor = kv2.second;
        if (neighborName == myNodeName
            || nextHopNodes.find(neighborName) != nextHopNodes.end()) {
          continue;
        }
        Metric distanceFromNeighbor = std::numeric_limits<Metric>::max();
        for (const auto& node : nodesWithPrefix) {
          try {
            const auto d = shortestPathsFromNeighbor.at(node).first;
            if (distanceFromNeighbor > d) {
              distanceFromNeighbor = d;
            }
          } catch (std::out_of_range const& e) {
            LOG(WARNING) << "No path to " << node << " from neighbor "
                         << neighborName;
          }
        }
        Metric neighborToHere =
            shortestPathsFromNeighbor.at(myNodeName).first;
        // This is the LFA condition per RFC 5286
        if (distanceFromNeighbor < prefixMetric + neighborToHere) {
          nextHopNodes[neighborName] = distanceFromNeighbor;
        }
      }
    }

    std::vector<thrift::Path> paths;
    for (const auto& link : linkState_.linksFromNode(myNodeName)) {
      const auto search = nextHopNodes.find(link->getOtherNodeName(myNodeName));
      if (search != nextHopNodes.end() && !link->isOverloaded()) {
        Metric distOverLink =
            link->getMetricFromNode(myNodeName) + search->second;
        if (computeLfaPaths_ || distOverLink == prefixMetric) {
          // if we are computing LFA paths, any nexthop to the node will do
          // otherwise, we only want those nexthops along a shortest path
          paths.emplace_back(
            apache::thrift::FRAGILE,
            isV4Prefix ? link->getNhV4FromNode(myNodeName) :
                         link->getNhV6FromNode(myNodeName),
            link->getIfaceFromNode(myNodeName),
            distOverLink);
        }
      }
    }
    routeDb.routes.emplace_back(
      apache::thrift::FRAGILE, prefix, std::move(paths));
  } // for prefixes_

  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "Decision::buildRouteDb took " << deltaTime.count() << "ms.";
  tData_.addStatValue(
      "decision.spf.buildroute_ms", deltaTime.count(), fbzmq::AVG);
  return routeDb;
} // buildRouteDb

Metric
SpfSolver::SpfSolverImpl::findMinDistToNeighbor(
    const std::string& myNodeName, const std::string& neighborName) {
  Metric min = std::numeric_limits<Metric>::max();
  for (const auto& link : linkState_.linksFromNode(myNodeName)) {
    if (!link->isOverloaded() &&
        link->getOtherNodeName(myNodeName) == neighborName) {
      min = std::min(link->getMetricFromNode(myNodeName), min);
    }
  }
  return min;
}

folly::Optional<Link>
SpfSolver::SpfSolverImpl::maybeMakeLink(
    const std::string& nodeName, const thrift::Adjacency& adj) {
  // only return Link if it is bidirectional.
  auto search = adjacencyDatabases_.find(adj.otherNodeName);
  if (search != adjacencyDatabases_.end()) {
    for (const auto& otherAdj : search->second.adjacencies) {
      if (nodeName == otherAdj.otherNodeName
          && adj.otherIfName == otherAdj.ifName
          && adj.ifName == otherAdj.otherIfName) {
        return Link(nodeName, adj, adj.otherNodeName, otherAdj);
      }
    }
  }
  return folly::none;
}

std::set<Link>
SpfSolver::SpfSolverImpl::getOrderedLinkSet(
    const thrift::AdjacencyDatabase& adjDb) {
  std::set<Link> links;
  for (const auto& adj : adjDb.adjacencies) {
    auto maybeLink = maybeMakeLink(adjDb.thisNodeName, adj);
    if (maybeLink) {
      links.emplace(maybeLink.value());
    }
  }
  return links;
}

std::unordered_map<std::string, int64_t>
SpfSolver::SpfSolverImpl::getCounters() {
  return tData_.getCounters();
}

//
// Public SpfSolver
//

SpfSolver::SpfSolver(
    const std::string& myNodeName, bool enableV4, bool computeLfaPaths)
    : impl_(
        new SpfSolver::SpfSolverImpl(myNodeName, enableV4, computeLfaPaths)) {}

SpfSolver::~SpfSolver() {}

// update adjacencies for the given router; everything is replaced
std::pair<bool /* topology has changed*/,
          bool /* local nextHop addrs have changed */>
SpfSolver::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb) {
  return impl_->updateAdjacencyDatabase(newAdjacencyDb);
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

folly::Optional<thrift::RouteDatabase>
SpfSolver::buildPaths(const std::string& myNodeName) {
  return impl_->buildPaths(myNodeName);
}

folly::Optional<thrift::RouteDatabase>
SpfSolver::buildRouteDb(const std::string& myNodeName) {
  return impl_->buildRouteDb(myNodeName);
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
    bool computeLfaPaths,
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
  spfSolver_ =
      std::make_unique<SpfSolver>(myNodeName_, enableV4, computeLfaPaths);

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  prepare(zmqContext);
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
        VLOG(3) << "Decision: request received...";
        processRequest();
      });

  auto zmqContextPtr = &zmqContext;
  scheduleTimeout(
      std::chrono::milliseconds(500),
      [this, zmqContextPtr] { initialSync(*zmqContextPtr); }
  );
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

    auto maybeRouteDb = spfSolver_->buildPaths(nodeName);
    if (maybeRouteDb.hasValue()) {
      reply.routeDb = std::move(maybeRouteDb.value());
    } else {
      reply.routeDb.thisNodeName = nodeName;
    }
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

ProcessPublicationResult
Decision::processPublication(thrift::Publication const& thriftPub) {
  ProcessPublicationResult res;

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
    folly::split(
      Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

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
        auto rc = spfSolver_->updateAdjacencyDatabase(adjacencyDb);
        if (rc.first) {
          res.adjChanged = true;
          pendingAdjUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
        }
        if (rc.second) {
          res.prefixesChanged = true;
          pendingPrefixUpdates_.addUpdate(myNodeName_, adjacencyDb.perfEvents);
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
    folly::split(
      Constants::kPrefixNameSeparator.toString(), key, prefix, nodeName);

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
Decision::submitCounters() {
  VLOG(3) << "Submitting counters...";

  // Prepare for submitting counters
  auto counters = spfSolver_->getCounters();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
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
      Constants::kEventLogCategory.toString(),
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
      Constants::kEventLogCategory.toString(),
      {sample.toJson()}));
}

void
Decision::processPendingUpdates() {
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
Decision::processPendingAdjUpdates() {
  VLOG(1) << "Decision: processing " << pendingAdjUpdates_.getCount()
          << " accumulated adjacency updates.";

  if (!pendingAdjUpdates_.getCount()) {
    LOG(ERROR) << "Decision route computation triggered without any pending "
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
  LOG(INFO) << "Decision: computing new paths.";
  auto maybeRouteDb = spfSolver_->buildPaths(myNodeName_);
  if (not maybeRouteDb.hasValue()) {
    LOG(WARNING) << "AdjacencyDb updates incurred no route updates";
    return;
  }

  auto& routeDb = maybeRouteDb.value();
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
Decision::processPendingPrefixUpdates() {
  auto maybePerfEvents = pendingPrefixUpdates_.getPerfEvents();
  pendingPrefixUpdates_.clear();

  // update routeDb once for all updates received
  LOG(INFO) << "Decision: updating new routeDb.";
  auto maybeRouteDb = spfSolver_->buildRouteDb(myNodeName_);
  if (not maybeRouteDb.hasValue()) {
    LOG(WARNING) << "PrefixDb updates incurred no route updates";
    return;
  }

  auto& routeDb = maybeRouteDb.value();
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
