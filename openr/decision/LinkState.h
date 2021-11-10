/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

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

  void operator=(T val);

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
      const std::string& area,
      const std::string& nodeName1,
      const std::string& if1,
      const std::string& nodeName2,
      const std::string& if2);

  Link(
      const std::string& area,
      const std::string& nodeName1,
      const openr::thrift::Adjacency& adj1,
      const std::string& nodeName2,
      const openr::thrift::Adjacency& adj2);

 private:
  const std::string area_;
  const std::string n1_, n2_, if1_, if2_;
  HoldableValue<LinkStateMetric> metric1_{1}, metric2_{1};
  HoldableValue<bool> overload1_{false}, overload2_{false};
  int32_t adjLabel1_{0}, adjLabel2_{0};
  // Weight represents a link's capacity (ex: 100Gbps)
  int64_t weight1_, weight2_;
  thrift::BinaryAddress nhV41_, nhV42_, nhV61_, nhV62_;
  LinkStateMetric holdUpTtl_{0};

  const std::pair<
      std::pair<std::string, std::string>,
      std::pair<std::string, std::string>>
      orderedNames_;

 public:
  const size_t hash{0};

  void setHoldUpTtl(LinkStateMetric ttl);

  bool isUp() const;

  bool decrementHolds();

  bool hasHolds() const;

  const std::string&
  getArea() const {
    return area_;
  }

  const std::string& getOtherNodeName(const std::string& nodeName) const;

  const std::string& firstNodeName() const;

  const std::string& secondNodeName() const;

  const std::string& getIfaceFromNode(const std::string& nodeName) const;

  LinkStateMetric getMetricFromNode(const std::string& nodeName) const;

  int32_t getAdjLabelFromNode(const std::string& nodeName) const;

  int64_t getWeightFromNode(const std::string& nodeName) const;

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

  void setWeightFromNode(const std::string& nodeName, int64_t weight);

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
  explicit LinkState(const std::string& area);

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

  // Class holding a network node's SPF result. and useful apis to get and set
  //   - nexthops toward the node
  //   - ultimate link and previous nodes on shortest paths towards node
  class NodeSpfResult {
   public:
    // Represents a link in a path towards a node. For an SPF result, we can use
    // these to trace paths back to the source from any connected node
    class PathLink {
     public:
      PathLink(std::shared_ptr<Link> const& l, std::string const& n)
          : link(l), prevNode(n) {}
      std::shared_ptr<Link> const link;
      std::string const prevNode;
    };

    explicit NodeSpfResult(LinkStateMetric m) : metric_(m) {}

    void
    reset(LinkStateMetric newMetric) {
      metric_ = newMetric;
      pathLinks_.clear();
      nextHops_.clear();
    }

    std::vector<PathLink> const&
    pathLinks() const {
      return pathLinks_;
    }
    std::unordered_set<std::string> const&
    nextHops() const {
      return nextHops_;
    }

    LinkStateMetric
    metric() const {
      return metric_;
    }

    void
    addPath(std::shared_ptr<Link> const& link, std::string const& prevNode) {
      pathLinks_.emplace_back(link, prevNode);
    }

    void
    addNextHops(std::unordered_set<std::string> const& toInsert) {
      nextHops_.insert(toInsert.begin(), toInsert.end());
    }

    void
    addNextHop(std::string const& toInsert) {
      nextHops_.insert(toInsert);
    }

   private:
    LinkStateMetric metric_{std::numeric_limits<LinkStateMetric>::max()};
    std::vector<PathLink> pathLinks_;
    std::unordered_set<std::string> nextHops_;
  };

  using SpfResult =
      std::unordered_map<std::string /* otherNodeName */, NodeSpfResult>;

  // Class which holds the UCMP results for a specific node which is part of the
  // SPF graph from a root node to a list of weighted leaf nodes. The class
  // holds the following node UCMP results.
  //   - Node's next-hop weights
  //   - Node's advertised weight
  class NodeUcmpResult {
   public:
    class NextHopLink {
     public:
      NextHopLink(
          std::shared_ptr<Link> const& l, std::string const& n, int64_t w)
          : link(l), nextHopNode(n), weight(w) {}
      std::shared_ptr<Link> const link;
      std::string const nextHopNode;
      int64_t weight{0};
    };

    explicit NodeUcmpResult() {}

    std::unordered_map<std::string, NextHopLink> const&
    nextHopLinks() const {
      return nextHopLinks_;
    }

    std::optional<int64_t>
    weight() const {
      return weight_;
    }

    void
    addNextHopLink(
        const std::string& localIface,
        std::shared_ptr<Link> const& link,
        std::string const& nextHopNode,
        int64_t weight) {
      nextHopLinks_.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(localIface),
          std::forward_as_tuple(link, nextHopNode, weight));
    }

    void
    setWeight(int64_t weight) {
      weight_ = weight;
    }

   private:
    std::unordered_map<std::string, NextHopLink> nextHopLinks_;
    std::optional<int64_t> weight_{std::nullopt};
  };

  using UcmpResult = std::unordered_map<std::string, NodeUcmpResult>;

  using Path = std::vector<std::shared_ptr<Link>>;

  // Shortest paths API:
  // - getSpfResult()
  // - getKthPaths()
  //
  // each is memoized all params. memoization invalidated for any topolgy
  // altering calls, i.e. if decrementHolds(), updateAdjacencyDatabase(), or
  // deleteAdjacencyDatabase() returns with LinkState::topologyChanged set true
  SpfResult const& getSpfResult(
      const std::string& nodeName, bool useLinkMetric = true) const;

  // API to resolve UCMP weights for all node's on the shortest path
  // between a root node and a list of weighted leaf nodes.
  UcmpResult resolveUcmpWeights(
      const SpfResult& spfGraph,
      const std::unordered_map<std::string, int64_t>& dstWeights,
      thrift::PrefixForwardingAlgorithm algo,
      bool useLinkMetric = true) const;

 private:
  // LinkState belongs to a unique area
  const std::string area_;

  // memoization structure for getSpfResult()
  mutable std::unordered_map<
      std::pair<std::string /* nodeName */, bool /* useLinkMetric */>,
      SpfResult>
      spfResults_;

 public:
  // Trace edge-disjoint paths from dest to src.
  // I.e., no two paths returned from this function can share any links
  //
  // Assertion: k >= 1
  // For k = 1, the above algorithm is perfomered considering all links in the
  // network.
  // For k > 1, the algorithm is performed considering all links except links on
  // paths in the set {p in getKthPaths(src, dest, i) | 1 <= i < k}.
  std::vector<LinkState::Path> const& getKthPaths(
      const std::string& src, const std::string& dest, size_t k) const;

 private:
  // memoization structure for getKthPaths()
  mutable std::unordered_map<
      std::tuple<std::string /* src */, std::string /* dest */, size_t /* k */>,
      std::vector<LinkState::Path>>
      kthPathResults_;

 public:
  // non-const public methods
  // IMPT: clear memoization structures as appropirate in these functions
  class LinkStateChange {
   public:
    LinkStateChange() = default;

    LinkStateChange(bool topo, bool link, bool node)
        : topologyChanged(topo),
          linkAttributesChanged(link),
          nodeLabelChanged(node) {}

    bool
    operator==(LinkStateChange const& other) const {
      return topologyChanged == other.topologyChanged &&
          linkAttributesChanged == other.linkAttributesChanged &&
          nodeLabelChanged == other.nodeLabelChanged;
    }

    // Whether topology has changed
    bool topologyChanged{false};
    // Newly added links in the topology. Today it is only populated in
    // `updateAdjacencyDatabase()`.
    std::vector<std::shared_ptr<Link>> addedLinks;
    // Whether attributes of links have changed
    bool linkAttributesChanged{false};
    // Whehter node labels have changed
    bool nodeLabelChanged{false};
  };

  LinkStateChange decrementHolds();

  // update adjacencies for the given router
  LinkStateChange updateAdjacencyDatabase(
      thrift::AdjacencyDatabase const& adjacencyDb,
      LinkStateMetric holdUpTtl = 0,
      LinkStateMetric holdDownTtl = 0);

  // delete a node's adjacency database
  // return true if this has caused any change in graph
  LinkStateChange deleteAdjacencyDatabase(const std::string& nodeName);

  // const public methods

  // returns metric from a to b,
  // if nodes b is not reachable from a, returns std::nullopt
  std::optional<LinkStateMetric> getMetricFromAToB(
      std::string const& a,
      std::string const& b,
      bool useLinkMetric = true) const;

  const std::string&
  getArea() const {
    return area_;
  }

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

  // check if path A is part of path B.
  // Example:
  // path A: a->b->c
  // path B: d->a->b->c->d
  // return True
  static bool
  pathAInPathB(Path const& a, Path const& b) {
    if (a.size() <= b.size()) {
      for (size_t i = 0; i < (b.size() - a.size()) + 1; ++i) {
        size_t a_i = 0, b_i = i;
        while (a_i < a.size() && *a.at(a_i) == *b.at(b_i)) {
          ++a_i;
          ++b_i;
        }
        if (a.size() == a_i) {
          return true;
        }
      }
    }
    return false;
  }

 private:
  // helpers to update the link state graph

  // find one path from dest to src for a given SpfResult
  // ingnore links already in linksToIgnore
  std::optional<Path> traceOnePath(
      std::string const& src,
      std::string const& dest,
      SpfResult const& result,
      LinkSet& linksToIgnore) const;

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
          {} /* optionaly specify a set of links to not use when running */)
      const;

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

// Classes needed for running Dijkstra to build an SPF graph starting at a root
// node to all other nodes the link state topology. In addition to implementing
// the priority queue element at the heart of Dijkstra's algorithm, this
// structure also allows us to store appication specfic data: nexthops.
class DijkstraQSpfNode {
 public:
  DijkstraQSpfNode(const std::string& n, LinkStateMetric m)
      : nodeName(n), result(m) {}

  LinkStateMetric
  metric() {
    return result.metric();
  }

  const std::string nodeName{""};
  LinkState::NodeSpfResult result;
};

// Dijkstra queue element used to derive UCMP weights for all node's
// on the shortest path between a root node and a list of weighted lead nodes
class DijkstraQUcmpNode {
 public:
  DijkstraQUcmpNode(const std::string& n, LinkStateMetric m)
      : nodeName(n), metric_(m) {}

  LinkStateMetric
  metric() {
    return metric_;
  }

  const std::string nodeName{""};
  LinkStateMetric metric_{0};
  LinkState::NodeUcmpResult result;
};

// Dijkstra Q template class.
// Implements a priority queue using a heap.
//
// Template object must have the following elements
//   - metric
//   - nodeName
template <class T>
class DijkstraQ {
 private:
  std::vector<std::shared_ptr<T>> heap_;
  std::unordered_map<std::string, std::shared_ptr<T>> nameToNode_;

  struct {
    bool
    operator()(std::shared_ptr<T> a, std::shared_ptr<T> b) const {
      if (a->metric() != b->metric()) {
        return a->metric() > b->metric();
      }
      return a->nodeName > b->nodeName;
    }
  } DijkstraQNodeGreater;

 public:
  void
  insertNode(const std::string& nodeName, LinkStateMetric d) {
    heap_.emplace_back(std::make_shared<T>(nodeName, d));
    nameToNode_[nodeName] = heap_.back();
    std::push_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
  }

  std::shared_ptr<T>
  get(const std::string& nodeName) {
    if (nameToNode_.count(nodeName)) {
      return nameToNode_.at(nodeName);
    }
    return nullptr;
  }

  std::shared_ptr<T>
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
  reMake() {
    // this is a bit slow but is rarely called in our application. In fact,
    // in networks where the metric is hop count, this will never be called
    // and the Dijkstra run is no different than BFS
    std::make_heap(heap_.begin(), heap_.end(), DijkstraQNodeGreater);
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
