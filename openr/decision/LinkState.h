/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

using LinkStateMetric = uint64_t;

inline bool
adjUsable(const thrift::Adjacency& adj, const std::string& nodeName) {
  // if onlyUsedByOther is set, then only the node that is "other" can
  // use this adj

  // Motivation: when neighbor A is initializing, we want to prevent routing
  // traffic to/through A, but we want to let A to route through us (B). Thus,
  // before A is fully initialized, we will have adj {self: B, other: A,
  // adjOnlyUsedByOtherNode: true} this prevents all nodes except A using this
  // adj

  // Case1: we are adjacent to an initilizing node, returns false (other !=
  //      self)
  // Case2: we are the initilizing node and neighbor is not, return true;
  //      (other == self)
  // Case3: we are not adjacent to the initilizing node, return
  //      false; (other != self)
  // Case4: Initilized: return true;
  if (*adj.adjOnlyUsedByOtherNode() && *adj.otherNodeName() != nodeName) {
    return false;
  }
  return true;
}

/*
 * Why define Link and LinkState? Isn't link state fully captured by something
 * like folly::F14FastMap<std::string, thrift::AdjacencyDatabase>?
 *
 * The answer is YES, but these classes provide a few major benefits over that
 * simple structure:
 *
 * 1. Only stores bidirectional links. i.e. for a link, the node at both ends
 * is advertising the adjancecy
 *
 * 2. Defines a hash and comparators that operate on the essential property of a
 * network link: the tuple: unorderedPair<orderedPair<nodeName, ifName>,
 *                                orderedPair<nodeName, ifName>>
 *
 * 3. For each unique link in the network, holds a single object that can be
 * quickly accessed and modified via the nodeName of either end of the link.
 *
 * 4. Provides useful apis to read and write link state.
 *
 * 5. Provides SPF results and handles memoizing this expensive computation
 * while the link state has not changed.
 */
class Link {
 public:
  Link(
      const std::string& area,
      const std::string& nodeName1,
      const std::string& if1,
      const std::string& nodeName2,
      const std::string& if2,
      bool usable = true);

  Link(
      const std::string& area,
      const std::string& nodeName1,
      const openr::thrift::Adjacency& adj1,
      const std::string& nodeName2,
      const openr::thrift::Adjacency& adj2,
      bool usable = true);

 private:
  // link can only belongs to one specific area
  const std::string area_;

  // node names and link names from both ends
  const std::string n1_, n2_, if1_, if2_;

  // metric from both ends
  LinkStateMetric metric1_{1}, metric2_{1};

  // interface overload(Hard-drain) state
  bool overload1_{false}, overload2_{false};

  // whether link can be used due to device initilization state
  bool usable_{true};

  // adjacency label for segment routing use case
  int32_t adjLabel1_{0}, adjLabel2_{0};

  // weight represents a link's capacity (ex: 100Gbps)
  int64_t weight1_, weight2_;

  // link addresses including v4 and v6
  thrift::BinaryAddress nhV41_, nhV42_, nhV61_, nhV62_;

  const std::pair<
      std::pair<std::string, std::string>,
      std::pair<std::string, std::string>>
      orderedNames_;

 public:
  const size_t hash{0};

  /*
   * [Accessor Method]
   */
  inline bool
  isUp() const {
    return (!overload1_) && (!overload2_) && usable_;
  }

  inline const std::string&
  getArea() const {
    return area_;
  }

  inline const std::string&
  getOtherNodeName(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return n2_;
    }
    if (n2_ == nodeName) {
      return n1_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline const std::string&
  firstNodeName() const {
    return orderedNames_.first.first;
  }

  inline const std::string&
  secondNodeName() const {
    return orderedNames_.second.first;
  }

  inline const std::string&
  getIfaceFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return if1_;
    }
    if (n2_ == nodeName) {
      return if2_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline LinkStateMetric
  getMetricFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return metric1_;
    }
    if (n2_ == nodeName) {
      return metric2_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline LinkStateMetric
  getMaxMetric() const {
    return std::max(metric1_, metric2_);
  }

  inline int32_t
  getAdjLabelFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return adjLabel1_;
    }
    if (n2_ == nodeName) {
      return adjLabel2_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline int64_t
  getWeightFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return weight1_;
    }
    if (n2_ == nodeName) {
      return weight2_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline bool
  getOverloadFromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return overload1_;
    }
    if (n2_ == nodeName) {
      return overload2_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline const thrift::BinaryAddress&
  getNhV4FromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return nhV41_;
    }
    if (n2_ == nodeName) {
      return nhV42_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline const thrift::BinaryAddress&
  getNhV6FromNode(const std::string& nodeName) const {
    if (n1_ == nodeName) {
      return nhV61_;
    }
    if (n2_ == nodeName) {
      return nhV62_;
    }
    throw std::invalid_argument(nodeName);
  }

  inline bool
  getUsability() const {
    return usable_;
  }

  /*
   * [Mutator Method]
   */
  void setNhV4FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV4);

  void setNhV6FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV6);

  bool setMetricFromNode(const std::string& nodeName, LinkStateMetric d);

  void setAdjLabelFromNode(const std::string& nodeName, int32_t adjLabel);

  void setWeightFromNode(const std::string& nodeName, int64_t weight);

  bool setOverloadFromNode(const std::string& nodeName, bool overload);

  bool setLinkUsability(const Link& newLink);

  bool operator<(const Link& other) const;

  bool operator==(const Link& other) const;

  std::string toString() const;

  std::string directionalToString(const std::string& fromNode) const;

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
      folly::F14FastSet<std::shared_ptr<Link>, LinkPtrHash, LinkPtrEqual>;
}; // class Link

class LinkState {
 public:
  explicit LinkState(const std::string& area, const std::string& myNodeName);

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
    folly::F14FastSet<std::string> const&
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
    addNextHops(folly::F14FastSet<std::string> const& toInsert) {
      nextHops_.insert(toInsert.begin(), toInsert.end());
    }

    void
    addNextHop(std::string const& toInsert) {
      nextHops_.insert(toInsert);
    }

   private:
    LinkStateMetric metric_{std::numeric_limits<LinkStateMetric>::max()};
    std::vector<PathLink> pathLinks_;
    folly::F14FastSet<std::string> nextHops_;
  };

  using SpfResult =
      folly::F14FastMap<std::string /* otherNodeName */, NodeSpfResult>;

  using Path = std::vector<std::shared_ptr<Link>>;

  // Shortest paths API:
  // - getSpfResult()
  // - getKthPaths()
  //
  // each is memoized all params. memoization invalidated for any topolgy
  // altering calls, i.e. if pdateAdjacencyDatabase(), or
  // deleteAdjacencyDatabase() returns with LinkState::topologyChanged set true
  SpfResult const& getSpfResult(
      const std::string& nodeName, bool useLinkMetric = true) const;

 private:
  // LinkState belongs to a unique area
  const std::string area_;

  // Current node name
  const std::string myNodeName_;

  // memoization structure for getSpfResult()
  mutable folly::F14FastMap<
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
  mutable folly::F14FastMap<
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

  // update adjacencies for the given router
  LinkStateChange updateAdjacencyDatabase(
      thrift::AdjacencyDatabase const& adjacencyDb,
      std::string area,
      bool inInitialization = false);

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

  const Link::LinkSet& linksFromNode(const std::string& nodeName) const;

  bool isNodeOverloaded(const std::string& nodeName) const;

  uint64_t getNodeMetricIncrement(const std::string& nodeName) const;

  size_t
  numLinks() const {
    return allLinks_.size();
  }

  size_t
  numNodes() const {
    return linkMap_.size();
  }

  bool
  linkUsable(
      const thrift::Adjacency& adj1, const thrift::Adjacency& adj2) const {
    // link is usable, only if both adj are usable by us.
    return adjUsable(adj1, myNodeName_) && adjUsable(adj2, myNodeName_);
  }

  // get adjacency databases
  folly::
      F14FastMap<std::string /* nodeName */, thrift::AdjacencyDatabase> const&
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
      Link::LinkSet& linksToIgnore) const;

  void addLink(std::shared_ptr<Link> link);

  void removeLink(std::shared_ptr<Link> link);

  void removeNode(const std::string& nodeName);

  bool updateNodeOverloaded(const std::string& nodeName, bool isOverloaded);

  std::string mayHaveLinkEventPropagationTime(
      thrift::AdjacencyDatabase const& adjDb,
      const std::string& ifName,
      bool isUp,
      bool firstPub,
      bool inInitialization);

  /*
   * [Dijkstra's Shortest-Path-First(SPF) Algorithm]
   *
   * Run SPF algorithm on the link-state graph.
   *
   * @param: src - the source node for the SPF run.
   * @param: useLinkMetric - if set, the algorithm will respect adjacency weight
   *                         advertised from the adjacent nodes, otherwise it
   *                         will consider the graph unweighted.
   * @param: linksToIgnore - optionally specify a set of links to not use when
   *                         running SPF. By default, this is an empty set.
   *
   * @return: SpfResult - a map of node -> NodeSpfResult obj mapping
   */
  SpfResult runSpf(
      const std::string& src,
      bool useLinkMetric,
      const Link::LinkSet& linksToIgnore = {}) const;

  /*
   * Util method to create Link object:
   *  - only if the bi-directional(reverse) adjacency is present
   *  - otherwise returns nullptr
   */
  std::shared_ptr<Link> maybeMakeLink(
      const std::string& nodeName, const thrift::Adjacency& adj) const;

  std::vector<std::shared_ptr<Link>> getOrderedLinkSet(
      const thrift::AdjacencyDatabase& adjDb) const;

  std::vector<std::shared_ptr<Link>> orderedLinksFromNode(
      const std::string& nodeName) const;

  // this stores the same link object accessible from either nodeName
  folly::F14FastMap<std::string /* nodeName */, Link::LinkSet> linkMap_;

  // useful for iterating over all the links
  Link::LinkSet allLinks_;

  // [hard-drain]
  folly::F14FastMap<std::string /* nodeName */, bool> nodeOverloads_;

  // [soft-drain]
  // track nodeMetricInc per node, 0 means not softdrained. Higher the value,
  // less it is preferred
  folly::F14FastMap<std::string /* nodeName */, uint64_t>
      nodeMetricIncrementVals_;

  // the latest AdjacencyDatabase we've received from each node
  folly::F14FastMap<std::string, thrift::AdjacencyDatabase> adjacencyDatabases_;

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

  const std::string nodeName;
  LinkState::NodeSpfResult result;
};

/*
 * Dijkstra Q template class.
 *
 * Implements a priority queue using a heap.
 *
 * Template object must have the following elements
 *  - metric
 *  - nodeName
 */
template <class T>
class DijkstraQ {
 private:
  std::vector<std::shared_ptr<T>> heap_;
  folly::F14FastMap<std::string, std::shared_ptr<T>> nameToNode_;

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
struct hash<openr::Link::LinkSet> {
  size_t operator()(openr::Link::LinkSet const& set) const;
};

template <>
struct equal_to<openr::Link::LinkSet> {
  bool operator()(
      openr::Link::LinkSet const& a, openr::Link::LinkSet const& b) const;
};

} // namespace std
