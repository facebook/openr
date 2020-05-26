/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/Network_types.h>

namespace openr {

using LinkStateMetric = uint64_t;

// HoldableValue is the basic building block for ordered FIB programming
// (rfc 6976)
//
// updateValue() will cause the previous value to be held for ttl
// time, where the ttl is chosen based on if the update is an up or down event.
// Subsequent updateValue() calls with the same value are no-ops.
// An update call with a new value when hasHold() is true results in the hold
// being clearted, thus changeing value().
//
// value() will return the held value until decrementTtl() returns true and the
// held value is cleared.

template <class T>
class HoldableValue {
 public:
  explicit HoldableValue(T val);

  const T& value() const;

  bool hasHold() const;

  // these methods return true if the call results in the value changing
  bool decrementTtl();
  bool updateValue(
      T val, LinkStateMetric holdUpTtl, LinkStateMetric holdDownTtl);

 private:
  bool isChangeBringingUp(T val);

  T val_;
  std::optional<T> heldVal_;
  LinkStateMetric holdTtl_{0};
};

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
// 5. Provides Shortest path results and handles memoizing this expesive
// computation while the link state has not changed
//

class Link {
 public:
  Link(
      const std::string& nodeName1,
      const openr::thrift::Adjacency& adj1,
      const std::string& nodeName2,
      const openr::thrift::Adjacency& adj2);

 private:
  const std::string n1_, n2_, if1_, if2_;
  HoldableValue<LinkStateMetric> metric1_{1}, metric2_{1};
  HoldableValue<bool> overload1_{false}, overload2_{false};
  int32_t adjLabel1_{0}, adjLabel2_{0};
  thrift::BinaryAddress nhV41_, nhV42_, nhV61_, nhV62_;
  LinkStateMetric holdUpTtl_{0};

  const std::pair<
      std::pair<std::string, std::string>,
      std::pair<std::string, std::string>>
      orderedNames;

 public:
  const size_t hash{0};

  void setHoldUpTtl(LinkStateMetric ttl);

  bool isUp() const;

  bool decrementHolds();

  bool hasHolds() const;

  const std::string& getOtherNodeName(const std::string& nodeName) const;

  const std::string& firstNodeName() const;

  const std::string& secondNodeName() const;

  const std::string& getIfaceFromNode(const std::string& nodeName) const;

  LinkStateMetric getMetricFromNode(const std::string& nodeName) const;

  int32_t getAdjLabelFromNode(const std::string& nodeName) const;

  bool getOverloadFromNode(const std::string& nodeName) const;

  const thrift::BinaryAddress& getNhV4FromNode(
      const std::string& nodeName) const;

  const thrift::BinaryAddress& getNhV6FromNode(
      const std::string& nodeName) const;

  void setNhV4FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV4);

  void setNhV6FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV6);

  bool setMetricFromNode(
      const std::string& nodeName,
      LinkStateMetric d,
      LinkStateMetric holdUpTtl,
      LinkStateMetric holdDownTtl);

  void setAdjLabelFromNode(const std::string& nodeName, int32_t adjLabel);

  bool setOverloadFromNode(
      const std::string& nodeName,
      bool overload,
      LinkStateMetric holdUpTtl,
      LinkStateMetric holdDownTtl);

  bool operator<(const Link& other) const;

  bool operator==(const Link& other) const;

  std::string toString() const;

  std::string directionalToString(const std::string& fromNode) const;
}; // class Link

class LinkState {
 public:
  struct LinkPtrHash {
    size_t operator()(const std::shared_ptr<Link>& l) const;
  };

  struct LinkPtrLess {
    bool operator()(
        const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const;
  };

  struct LinkPtrEqual {
    bool operator()(
        const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const;
  };

  using LinkSet =
      std::unordered_set<std::shared_ptr<Link>, LinkPtrHash, LinkPtrEqual>;

  using SpfResult = std::unordered_map<
      std::string /* otherNodeName */,
      std::pair<
          LinkStateMetric,
          std::unordered_set<std::string /* nextHopNodeName */>>>;

  // non-const public methods
  // IMPT: clear spfResults_ as appropirate in these functions

  SpfResult const& getSpfResult(
      const std::string& nodeName,
      bool useLinkMetric = true,
      LinkSet const& linksToIgnore = {});

  bool decrementHolds();

  // update adjacencies for the given router
  std::pair<
      bool /* topology has changed */,
      bool /* adjacency attributes have changed: nexthop addr, or label */>
  updateAdjacencyDatabase(
      thrift::AdjacencyDatabase const& adjacencyDb,
      LinkStateMetric holdUpTtl,
      LinkStateMetric holdDownTtl);

  // delete a node's adjacency database
  // return true if this has caused any change in graph
  bool deleteAdjacencyDatabase(const std::string& nodeName);

  // const public methods

  bool
  hasNode(const std::string& nodeName) const {
    return 0 != adjacencyDatabases_.count(nodeName);
  }

  const LinkSet& linksFromNode(const std::string& nodeName) const;

  bool isNodeOverloaded(const std::string& nodeName) const;

  bool hasHolds() const;

  size_t
  numLinks() const {
    return allLinks_.size();
  }

  size_t
  numNodes() const {
    return linkMap_.size();
  }

  // get adjacency databases
  std::unordered_map<
      std::string /* nodeName */,
      thrift::AdjacencyDatabase> const&
  getAdjacencyDatabases() const {
    return adjacencyDatabases_;
  }

 private:
  // helpers to update the link state graph
  void addLink(std::shared_ptr<Link> link);

  void removeLink(std::shared_ptr<Link> link);

  void removeNode(const std::string& nodeName);

  bool updateNodeOverloaded(
      const std::string& nodeName,
      bool isOverloaded,
      LinkStateMetric holdUpTtl,
      LinkStateMetric holdDownTtl);

  // run Dijkstra's Shortest Path First algorithm on the link state graph
  SpfResult runSpf(
      const std::string& src, /* the source node for the SPF run */
      bool useLinkMetric, /* if set, the algorithm will respect adjancecy
                             weights as advertised from the adjacent nodes,
                             otherwise it will consider the graph unweighted */
      const LinkSet& linksToIgnore =
          {} /* optionaly specify a set of links to not use when running */);

  // memoization structure for spf runs
  std::unordered_map<std::tuple<std::string, bool, LinkSet>, SpfResult>
      spfResults_;

  // returns Link object if the reverse adjancency is present in
  // adjacencyDatabases_.at(adj.otherNodeName), else returns nullptr
  std::shared_ptr<Link> maybeMakeLink(
      const std::string& nodeName, const thrift::Adjacency& adj) const;

  std::vector<std::shared_ptr<Link>> getOrderedLinkSet(
      const thrift::AdjacencyDatabase& adjDb) const;

  std::vector<std::shared_ptr<Link>> orderedLinksFromNode(
      const std::string& nodeName) const;

  // this stores the same link object accessible from either nodeName
  std::unordered_map<std::string /* nodeName */, LinkSet> linkMap_;

  // useful for iterating over all the links
  LinkSet allLinks_;

  std::unordered_map<std::string /* nodeName */, HoldableValue<bool>>
      nodeOverloads_;

  // the latest AdjacencyDatabase we've received from each node
  std::unordered_map<std::string, thrift::AdjacencyDatabase>
      adjacencyDatabases_;

}; // class LinkState

// Classes needed for running Dijkstra
// In addition to implementing the priority queue at the heart of Dijkstra's
// algorithm, this structure also allows us to store appication specfic data:
// nexthops.
class DijkstraQNode {
 public:
  DijkstraQNode(const std::string& n, LinkStateMetric d)
      : nodeName(n), distance(d) {}
  const std::string nodeName;
  LinkStateMetric distance{0};
  std::unordered_set<std::string> nextHops;
};

class DijkstraQ {
 private:
  std::vector<std::shared_ptr<DijkstraQNode>> heap_;
  std::unordered_map<std::string, std::shared_ptr<DijkstraQNode>> nameToNode_;

  struct {
    bool
    operator()(
        std::shared_ptr<DijkstraQNode> a,
        std::shared_ptr<DijkstraQNode> b) const {
      if (a->distance != b->distance) {
        return a->distance > b->distance;
      }
      return a->nodeName > b->nodeName;
    }
  } DijkstraQNodeGreater;

 public:
  void
  insertNode(const std::string& nodeName, LinkStateMetric d) {
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

  std::shared_ptr<DijkstraQNode>
  extractMin() {
    if (heap_.empty()) {
      return nullptr;
    }
    auto min = heap_.at(0);
    CHECK(nameToNode_.erase(min->nodeName));
    std::pop_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
    heap_.pop_back();
    return min;
  }

  void
  decreaseKey(const std::string& nodeName, LinkStateMetric d) {
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
} // namespace openr

namespace std {

// needed for certain containers

template <>
struct hash<openr::Link> {
  size_t operator()(openr::Link const& link) const;
};

template <>
struct hash<openr::LinkState::LinkSet> {
  size_t operator()(openr::LinkState::LinkSet const& set) const;
};

template <>
struct equal_to<openr::LinkState::LinkSet> {
  bool operator()(
      openr::LinkState::LinkSet const& a,
      openr::LinkState::LinkSet const& b) const;
};

} // namespace std
