/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>
#include <openr/common/LsdbUtil.h>
#include <openr/decision/LinkState.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

namespace fb303 = facebook::fb303;

size_t
std::hash<openr::Link>::operator()(openr::Link const& link) const {
  return link.hash;
}

bool
std::equal_to<openr::LinkState::LinkSet>::operator()(
    openr::LinkState::LinkSet const& a,
    openr::LinkState::LinkSet const& b) const {
  if (a.size() == b.size()) {
    for (auto const& i : a) {
      if (!b.count(i)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

size_t
std::hash<openr::LinkState::LinkSet>::operator()(
    openr::LinkState::LinkSet const& set) const {
  size_t hash = 0;
  for (auto const& link : set) {
    // Note: XOR is associative and communitive so we get a consitent hash no
    // matter the order of the set
    hash ^= std::hash<openr::Link>()(*link);
  }
  return hash;
}

namespace openr {

Link::Link(
    const std::string& area,
    const std::string& nodeName1,
    const std::string& if1,
    const std::string& nodeName2,
    const std::string& if2,
    bool usable)
    : area_(area),
      n1_(nodeName1),
      n2_(nodeName2),
      if1_(if1),
      if2_(if2),
      usable_(usable),
      orderedNames_(
          std::minmax(std::make_pair(n1_, if1_), std::make_pair(n2_, if2_))),
      hash(std::hash<std::pair<
               std::pair<std::string, std::string>,
               std::pair<std::string, std::string>>>()(orderedNames_)) {}

Link::Link(
    const std::string& area,
    const std::string& nodeName1,
    const openr::thrift::Adjacency& adj1,
    const std::string& nodeName2,
    const openr::thrift::Adjacency& adj2,
    bool usable)
    : Link(area, nodeName1, *adj1.ifName(), nodeName2, *adj2.ifName(), usable) {
  metric1_ = *adj1.metric();
  metric2_ = *adj2.metric();
  overload1_ = *adj1.isOverloaded();
  overload2_ = *adj2.isOverloaded();
  adjLabel1_ = *adj1.adjLabel();
  adjLabel2_ = *adj2.adjLabel();
  nhV41_ = *adj1.nextHopV4();
  nhV42_ = *adj2.nextHopV4();
  nhV61_ = *adj1.nextHopV6();
  nhV62_ = *adj2.nextHopV6();
  weight1_ = *adj1.weight();
  weight2_ = *adj2.weight();
}

void
Link::setNhV4FromNode(
    const std::string& nodeName, const thrift::BinaryAddress& nhV4) {
  if (n1_ == nodeName) {
    nhV41_ = nhV4;
  } else if (n2_ == nodeName) {
    nhV42_ = nhV4;
  } else {
    throw std::invalid_argument(nodeName);
  }
}

void
Link::setNhV6FromNode(
    const std::string& nodeName, const thrift::BinaryAddress& nhV6) {
  if (n1_ == nodeName) {
    nhV61_ = nhV6;
  } else if (n2_ == nodeName) {
    nhV62_ = nhV6;
  } else {
    throw std::invalid_argument(nodeName);
  }
}

bool
Link::setMetricFromNode(const std::string& nodeName, LinkStateMetric d) {
  if (n1_ == nodeName) {
    metric1_ = d;
    return true;
  } else if (n2_ == nodeName) {
    metric2_ = d;
    return true;
  }
  throw std::invalid_argument(nodeName);
}

void
Link::setAdjLabelFromNode(const std::string& nodeName, int32_t adjLabel) {
  if (n1_ == nodeName) {
    adjLabel1_ = adjLabel;
  } else if (n2_ == nodeName) {
    adjLabel2_ = adjLabel;
  } else {
    throw std::invalid_argument(nodeName);
  }
}

void
Link::setWeightFromNode(const std::string& nodeName, int64_t weight) {
  if (n1_ == nodeName) {
    weight1_ = weight;
  } else if (n2_ == nodeName) {
    weight2_ = weight;
  } else {
    throw std::invalid_argument(nodeName);
  }
}

bool
Link::setOverloadFromNode(const std::string& nodeName, bool overload) {
  bool const wasUp = isUp();
  if (n1_ == nodeName) {
    overload1_ = overload;
  } else if (n2_ == nodeName) {
    overload2_ = overload;
  } else {
    throw std::invalid_argument(nodeName);
  }
  // since we don't support simplex overloads, we only signal topo change if
  // this is true
  return wasUp != isUp();
}

bool
Link::setLinkUsability(const Link& newLink) {
  // copy newLink's usablity
  // make sure that they represent the same link
  CHECK(*this == newLink); // checking hash (ordered names are checked)
  bool wasUp = isUp();
  usable_ = newLink.usable_;
  return wasUp != isUp();
}

bool
Link::operator<(const Link& other) const {
  if (this->hash != other.hash) {
    return this->hash < other.hash;
  }
  return this->orderedNames_ < other.orderedNames_;
}

bool
Link::operator==(const Link& other) const {
  if (this->hash != other.hash) {
    return false;
  }
  return this->orderedNames_ == other.orderedNames_;
}

std::string
Link::toString() const {
  return fmt::format("{} - {}%{} <---> {}%{}", area_, n1_, if1_, n2_, if2_);
}

std::string
Link::directionalToString(const std::string& fromNode) const {
  return fmt::format(
      "{} - {}%{} ---> {}%{}",
      area_,
      fromNode,
      getIfaceFromNode(fromNode),
      getOtherNodeName(fromNode),
      getIfaceFromNode(getOtherNodeName(fromNode)));
}

LinkState::LinkState(const std::string& area, const std::string& myNodeName)
    : area_(area), myNodeName_(myNodeName) {}

size_t
LinkState::LinkPtrHash::operator()(const std::shared_ptr<Link>& l) const {
  return l->hash;
}

bool
LinkState::LinkPtrLess::operator()(
    const std::shared_ptr<Link>& lhs, const std::shared_ptr<Link>& rhs) const {
  return *lhs < *rhs;
}

bool
LinkState::LinkPtrEqual::operator()(
    const std::shared_ptr<Link>& lhs, const std::shared_ptr<Link>& rhs) const {
  return *lhs == *rhs;
}

std::optional<LinkState::Path>
LinkState::traceOnePath(
    std::string const& src,
    std::string const& dest,
    SpfResult const& result,
    LinkSet& linksToIgnore) const {
  if (src == dest) {
    return LinkState::Path{};
  }
  auto const& nodeResult = result.at(dest);
  for (auto const& pathLink : nodeResult.pathLinks()) {
    // only consider this link if we haven't yet
    if (linksToIgnore.insert(pathLink.link).second) {
      auto path = traceOnePath(src, pathLink.prevNode, result, linksToIgnore);
      if (path) {
        path->push_back(pathLink.link);
        return path;
      }
    }
  }
  return std::nullopt;
}

void
LinkState::addLink(std::shared_ptr<Link> link) {
  CHECK(linkMap_[link->firstNodeName()].insert(link).second);
  CHECK(linkMap_[link->secondNodeName()].insert(link).second);
  CHECK(allLinks_.insert(link).second);
}

// throws std::out_of_range if links are not present
void
LinkState::removeLink(std::shared_ptr<Link> link) {
  CHECK(linkMap_.at(link->firstNodeName()).erase(link));
  CHECK(linkMap_.at(link->secondNodeName()).erase(link));
  CHECK(allLinks_.erase(link));
}

void
LinkState::removeNode(const std::string& nodeName) {
  auto search = linkMap_.find(nodeName);
  if (search == linkMap_.end()) {
    // No links were added (addition of empty adjacency db can cause this)
    return;
  }

  // erase ptrs to these links from other nodes
  for (auto const& link : search->second) {
    try {
      CHECK(linkMap_.at(link->getOtherNodeName(nodeName)).erase(link));
      CHECK(allLinks_.erase(link));
    } catch (std::out_of_range const&) {
      XLOG(FATAL) << "std::out_of_range for " << nodeName;
    }
  }
  linkMap_.erase(search);
  nodeOverloads_.erase(nodeName);
}

const LinkState::LinkSet&
LinkState::linksFromNode(const std::string& nodeName) const {
  static const LinkState::LinkSet defaultEmptySet;
  auto search = linkMap_.find(nodeName);
  if (search != linkMap_.end()) {
    return search->second;
  }
  return defaultEmptySet;
}

std::vector<std::shared_ptr<Link>>
LinkState::orderedLinksFromNode(const std::string& nodeName) const {
  std::vector<std::shared_ptr<Link>> links;
  if (linkMap_.count(nodeName)) {
    links.insert(
        links.begin(),
        linkMap_.at(nodeName).begin(),
        linkMap_.at(nodeName).end());
    std::sort(links.begin(), links.end(), LinkPtrLess{});
  }
  return links;
}

bool
LinkState::updateNodeOverloaded(
    const std::string& nodeName, bool isOverloaded) {
  /*
   * As per `insert_or_assign`'s documentation:
   *
   * https://en.cppreference.com/w/cpp/container/unordered_map/insert_or_assign
   *
   * template<class M>
   * std::pair<iterator, bool>
   *
   * will be returned. The `bool` component is:
   *  - TRUE: if the insertion took place
   *  - FALSE: if the assignment took place
   */
  if (nodeOverloads_.count(nodeName) and
      nodeOverloads_.at(nodeName) == isOverloaded) {
    // don't indicate LinkState change for duplicate update
    return false;
  }

  const auto [_, inserted] =
      nodeOverloads_.insert_or_assign(nodeName, isOverloaded);
  // don't indicate LinkState changed if this is a new node
  return not inserted;
}

/*
 * @brief  If link status record exists for a passed link and
 *         if the tiemstamp is recorded for the link event,
 *         calculate link propagation time and log to fb303.
 *         Skip reporting of propagation time during Openr
 *         initialization
 *
 * @param adjDb     New adjacency database that may have link record
 * @param linkName  Name of the link for propagation time requested
 * @param isUp      new status of the link up or down
 * @param inInitialization if Openr currently being initialized,
 *                         in another words either kvstore and/or
 *                         local adjacencies sync is not yet complete
 *
 * @return std::string  a string result with calculated propagation time
 */
std::string
LinkState::mayHaveLinkEventPropagationTime(
    thrift::AdjacencyDatabase const& adjDb,
    const std::string& linkName,
    bool isUp,
    bool firstPub,
    bool inInitialization) {
  std::string propagationTimeLogStr = "";
  if (inInitialization) {
    return propagationTimeLogStr;
  }

  if (adjDb.linkStatusRecords().has_value()) { // link status
    auto linkStatus =
        adjDb.linkStatusRecords()->linkStatusMap()->find(linkName);
    // Ignore link propagation time for a first publication from peer
    // because links may have been up long before, but we are getting
    // them as part of first publication
    //
    // Timestamp is updated when link status changes (over Netlink). But
    // at router boot-up, link doesn't change status, and so timestamp is
    // not set and equals to 0 (default value). We ignore this situation.
    if (!firstPub &&
        (linkStatus != adjDb.linkStatusRecords()->linkStatusMap()->end()) &&
        *linkStatus->second.unixTs()) {
      int64_t propagationTime =
          getUnixTimeStampMs() - *linkStatus->second.unixTs();
      if (propagationTime >= 0) { // ignore negative delta (NTP issue)
        // log to fb303
        std::string key = fmt::format(
            "decision.linkstate.{}.propagation_time_ms", isUp ? "up" : "down");
        fb303::fbData->addStatValue(key, propagationTime, fb303::AVG);
        propagationTimeLogStr =
            fmt::format("propagation time {} ms", propagationTime);
      }
    }
  }
  return propagationTimeLogStr;
}

bool
LinkState::isNodeOverloaded(const std::string& nodeName) const {
  return nodeOverloads_.count(nodeName) and nodeOverloads_.at(nodeName);
}

std::uint64_t
LinkState::getNodeMetricIncrement(const std::string& nodeName) const {
  const auto it = nodeMetricIncrementVals_.find(nodeName);
  if (it != nodeMetricIncrementVals_.cend()) {
    return it->second;
  }
  return 0;
}

std::shared_ptr<Link>
LinkState::maybeMakeLink(
    const std::string& nodeName, const thrift::Adjacency& adj) const {
  // only return Link if it is bidirectional.
  auto search = adjacencyDatabases_.find(*adj.otherNodeName());
  if (search != adjacencyDatabases_.end()) {
    for (const auto& otherAdj : *search->second.adjacencies()) {
      if (nodeName == *otherAdj.otherNodeName() &&
          *adj.otherIfName() == *otherAdj.ifName() &&
          *adj.ifName() == *otherAdj.otherIfName()) {
        auto usable = linkUsable(adj, otherAdj);
        return std::make_shared<Link>(
            area_, nodeName, adj, *adj.otherNodeName(), otherAdj, usable);
      }
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<Link>>
LinkState::getOrderedLinkSet(const thrift::AdjacencyDatabase& adjDb) const {
  std::vector<std::shared_ptr<Link>> links;
  links.reserve(adjDb.adjacencies()->size());
  for (const auto& adj : *adjDb.adjacencies()) {
    auto linkPtr = maybeMakeLink(*adjDb.thisNodeName(), adj);
    if (nullptr != linkPtr) {
      links.emplace_back(linkPtr);
    }
  }
  links.shrink_to_fit();
  std::sort(links.begin(), links.end(), LinkState::LinkPtrLess{});
  return links;
}

LinkState::LinkStateChange
LinkState::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb,
    std::string area,
    bool inInitialization) {
  LinkStateChange change;

  // Area field must be specified and match with area_
  DCHECK_EQ(area_, area);
  for (auto const& adj : *newAdjacencyDb.adjacencies()) {
    XLOG(DBG3) << "  neighbor: " << *adj.otherNodeName()
               << ", remoteIfName: " << getRemoteIfName(adj)
               << ", ifName: " << *adj.ifName() << ", metric: " << *adj.metric()
               << ", overloaded: " << *adj.isOverloaded()
               << ", rtt: " << *adj.rtt() << ", weight: " << *adj.weight();
  }

  // Default construct if it did not exist
  auto const& nodeName = *newAdjacencyDb.thisNodeName();
  bool firstPub = false;
  if (adjacencyDatabases_.find(nodeName) == adjacencyDatabases_.end()) {
    firstPub = true;
  }
  thrift::AdjacencyDatabase priorAdjacencyDb(
      std::move(adjacencyDatabases_[nodeName]));
  // replace
  adjacencyDatabases_[nodeName] = newAdjacencyDb;

  // for comparing old and new state, we order the links based on the tuple
  // <nodeName1, iface1, nodeName2, iface2>, this allows us to easily discern
  // topology changes in the single loop below
  auto oldLinks = orderedLinksFromNode(nodeName);
  auto newLinks = getOrderedLinkSet(newAdjacencyDb);

  // fill these sets with the appropriate links
  std::unordered_set<Link> linksUp;
  std::unordered_set<Link> linksDown;

  // topology changed if a node is overloaded / un-overloaded
  change.topologyChanged |=
      updateNodeOverloaded(nodeName, *newAdjacencyDb.isOverloaded());

  // topology is changed if softdrain value is changed.
  change.topologyChanged |= *priorAdjacencyDb.nodeMetricIncrementVal() !=
      *newAdjacencyDb.nodeMetricIncrementVal();
  nodeMetricIncrementVals_.insert_or_assign(
      nodeName, *newAdjacencyDb.nodeMetricIncrementVal());

  change.nodeLabelChanged =
      *priorAdjacencyDb.nodeLabel() != *newAdjacencyDb.nodeLabel();

  auto newIter = newLinks.begin();
  auto oldIter = oldLinks.begin();
  while (newIter != newLinks.end() || oldIter != oldLinks.end()) {
    if (newIter != newLinks.end() &&
        (oldIter == oldLinks.end() || **newIter < **oldIter)) {
      // newIter is pointing at a Link not currently present, record this as a
      // link to add and advance newIter
      change.topologyChanged |= (*newIter)->isUp();
      // even if we are holding a change, we apply the change to our link state
      // and check for holds when running spf. this ensures we don't add the
      // same hold twice
      addLink(*newIter);
      change.addedLinks.emplace_back(*newIter);
      std::string propagationTimeStr = mayHaveLinkEventPropagationTime(
          newAdjacencyDb,
          (*newIter)->getIfaceFromNode(*newAdjacencyDb.thisNodeName()),
          true /* up */,
          firstPub,
          inInitialization);
      XLOG(DBG1) << fmt::format(
          "[LINK UP] {} [from {}] {}",
          (*newIter)->toString(),
          *newAdjacencyDb.thisNodeName(),
          propagationTimeStr);
      ++newIter;
      continue;
    }
    if (oldIter != oldLinks.end() &&
        (newIter == newLinks.end() || **oldIter < **newIter)) {
      // oldIter is pointing at a Link that is no longer present, record this
      // as a link to remove and advance oldIter.
      // If this link was previously overloaded or had a hold up, this does not
      // change the topology.
      change.topologyChanged |= (*oldIter)->isUp();
      removeLink(*oldIter);
      std::string propagationTimeStr = mayHaveLinkEventPropagationTime(
          newAdjacencyDb,
          (*oldIter)->getIfaceFromNode(*newAdjacencyDb.thisNodeName()),
          false /* down */,
          firstPub,
          inInitialization);
      XLOG(DBG1) << fmt::format(
          "[LINK DOWN] {} [from {}] {}",
          (*oldIter)->toString(),
          *newAdjacencyDb.thisNodeName(),
          propagationTimeStr);
      ++oldIter;
      continue;
    }
    // The newIter and oldIter point to the same link. This link did not go up
    // or down. The topology may still have changed though if the link overlaod
    // or metric changed
    auto& newLink = **newIter;
    auto& oldLink = **oldIter;

    // change the metric on the link object we already have
    if (newLink.getMetricFromNode(nodeName) !=
        oldLink.getMetricFromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] Metric change on link {}, {} -> {}",
          newLink.directionalToString(nodeName),
          oldLink.getMetricFromNode(nodeName),
          newLink.getMetricFromNode(nodeName));
      change.topologyChanged |= oldLink.setMetricFromNode(
          nodeName, newLink.getMetricFromNode(nodeName));
    }

    // Check if link is now usable / unusable
    auto isUp = newLink.isUp();
    auto wasUp = oldLink.isUp();
    if (isUp != wasUp) {
      XLOG(DBG1)
          << fmt::format("[LINK UPDATE] Link usability: {} -> {}", wasUp, isUp);
      change.topologyChanged |= oldLink.setLinkUsability(newLink);
    }

    if (newLink.getOverloadFromNode(nodeName) !=
        oldLink.getOverloadFromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] Overload change on link {}: {} -> {}",
          newLink.directionalToString(nodeName),
          oldLink.getOverloadFromNode(nodeName),
          newLink.getOverloadFromNode(nodeName));
      change.topologyChanged |= oldLink.setOverloadFromNode(
          nodeName, newLink.getOverloadFromNode(nodeName));
    }

    // Check if adjacency label has changed
    if (newLink.getAdjLabelFromNode(nodeName) !=
        oldLink.getAdjLabelFromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] AdjLabel change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          oldLink.getAdjLabelFromNode(nodeName),
          newLink.getAdjLabelFromNode(nodeName));

      change.linkAttributesChanged |= true;

      // change the adjLabel on the link object we already have
      oldLink.setAdjLabelFromNode(
          nodeName, newLink.getAdjLabelFromNode(nodeName));
    }

    // Check if link weight has changed
    if (newLink.getWeightFromNode(nodeName) !=
        oldLink.getWeightFromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] Weight change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          oldLink.getWeightFromNode(nodeName),
          newLink.getWeightFromNode(nodeName));

      change.linkAttributesChanged |= true;

      // change the weight on the link object we already have
      oldLink.setWeightFromNode(nodeName, newLink.getWeightFromNode(nodeName));
    }

    // check if local nextHops Changed
    if (newLink.getNhV4FromNode(nodeName) !=
        oldLink.getNhV4FromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] V4-NextHop address change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          toString(oldLink.getNhV4FromNode(nodeName)),
          toString(newLink.getNhV4FromNode(nodeName)));

      change.linkAttributesChanged |= true;
      oldLink.setNhV4FromNode(nodeName, newLink.getNhV4FromNode(nodeName));
    }
    if (newLink.getNhV6FromNode(nodeName) !=
        oldLink.getNhV6FromNode(nodeName)) {
      XLOG(DBG1) << fmt::format(
          "[LINK UPDATE] V6-NextHop address change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          toString(oldLink.getNhV6FromNode(nodeName)),
          toString(newLink.getNhV6FromNode(nodeName)));

      change.linkAttributesChanged |= true;
      oldLink.setNhV6FromNode(nodeName, newLink.getNhV6FromNode(nodeName));
    }
    ++newIter;
    ++oldIter;
  }
  if (change.topologyChanged) {
    spfResults_.clear();
    kthPathResults_.clear();
  }
  return change;
}

LinkState::LinkStateChange
LinkState::deleteAdjacencyDatabase(const std::string& nodeName) {
  LinkStateChange change;
  XLOG(DBG1) << "Deleting adjacency database for node " << nodeName;
  auto search = adjacencyDatabases_.find(nodeName);

  if (search != adjacencyDatabases_.end()) {
    removeNode(nodeName);
    adjacencyDatabases_.erase(search);
    spfResults_.clear();
    kthPathResults_.clear();
    change.topologyChanged = true;
  } else {
    XLOG(WARNING) << "Trying to delete adjacency db for non-existing node "
                  << nodeName;
  }
  return change;
}

std::optional<LinkStateMetric>
LinkState::getMetricFromAToB(
    std::string const& a, std::string const& b, bool useLinkMetric) const {
  if (a == b) {
    return 0;
  }
  auto const& spfResult = getSpfResult(a, useLinkMetric);
  if (spfResult.count(b)) {
    return spfResult.at(b).metric();
  }
  return std::nullopt;
}

std::vector<LinkState::Path> const&
LinkState::getKthPaths(
    const std::string& src, const std::string& dest, size_t k) const {
  CHECK_GE(k, 1);
  std::tuple<std::string, std::string, size_t> key(src, dest, k);
  auto entryIter = kthPathResults_.find(key);
  if (kthPathResults_.end() == entryIter) {
    LinkSet linksToIgnore;
    for (size_t i = 1; i < k; ++i) {
      for (auto const& path : getKthPaths(src, dest, i)) {
        for (auto const& link : path) {
          linksToIgnore.insert(link);
        }
      }
    }
    std::vector<LinkState::Path> paths;
    auto const& res = linksToIgnore.empty() ? getSpfResult(src, true)
                                            : runSpf(src, true, linksToIgnore);
    if (res.count(dest)) {
      LinkSet visitedLinks;
      auto path = traceOnePath(src, dest, res, visitedLinks);
      while (path && !path->empty()) {
        paths.push_back(std::move(*path));
        path = traceOnePath(src, dest, res, visitedLinks);
      }
    }
    entryIter = kthPathResults_.emplace(key, std::move(paths)).first;
  }
  return entryIter->second;
}

LinkState::SpfResult const&
LinkState::getSpfResult(
    const std::string& thisNodeName, bool useLinkMetric) const {
  std::pair<std::string, bool> key{thisNodeName, useLinkMetric};
  auto entryIter = spfResults_.find(key);
  if (spfResults_.end() == entryIter) {
    auto res = runSpf(thisNodeName, useLinkMetric);
    entryIter = spfResults_.emplace(std::move(key), std::move(res)).first;
  }
  return entryIter->second;
}

/**
 * Compute shortest-path routes from perspective of nodeName;
 */
LinkState::SpfResult
LinkState::runSpf(
    const std::string& thisNodeName,
    bool useLinkMetric,
    const LinkState::LinkSet& linksToIgnore) const {
  LinkState::SpfResult result;

  fb303::fbData->addStatValue("decision.spf_runs", 1, fb303::COUNT);
  const auto startTime = std::chrono::steady_clock::now();

  DijkstraQ<DijkstraQSpfNode> q;
  q.insertNode(thisNodeName, 0);
  while (auto node = q.extractMin()) {
    // we've found this node's shortest paths. record it
    auto emplaceRc = result.emplace(node->nodeName, std::move(node->result));
    CHECK(emplaceRc.second);

    auto const& recordedNodeName = emplaceRc.first->first;
    auto const recordedNodeMetric = emplaceRc.first->second.metric();
    auto const& recordedNodeNextHops = emplaceRc.first->second.nextHops();

    if (isNodeOverloaded(recordedNodeName) and
        recordedNodeName != thisNodeName) {
      /*
       * [Node Hard-Drain]
       *
       * No transit traffic through this node. We've recorded the nexthops to
       * this node, but will not consider any of it's adjancecies as offering
       * lower cost paths towards further away nodes. This effectively drains
       * traffic away from this node.
       */
      continue;
    }
    /*
     * We have the shortest path nexthops for `recordedNodeName`. Use these
     * nextHops for any node that is connected to `recordedNodeName` that
     * doesn't already have a lower cost path from thisNodeName.
     *
     * This is the "relax" step in the Dijkstra Algorithm pseudocode in CLRS.
     */
    for (const auto& link : linksFromNode(recordedNodeName)) {
      auto& otherNodeName = link->getOtherNodeName(recordedNodeName);
      if (!link->isUp() or result.count(otherNodeName) or
          linksToIgnore.count(link)) {
        /*
         * [Interface Hard-Drain]
         *
         * When interface is hard-drained, aka, with overload bit set,
         *
         * link->isUp() returns false
         *
         * with either side of the adjacency marked as `overloaded`.
         *
         * This prevents Dijkstra algorithm from considering this link.
         */
        continue;
      }
      /*
       * [Interface Soft-Drain]
       *
       * When interface soft-drain is issued, unlike interface hard-drain, which
       * will set "isOverloaded" bit inside thrift::Adjacency and excluded from
       * Dijkstra's SPF run(see comments in "Interface Hard-Drain"), it just
       * increase the "metric" inside thrift::Adjacency.
       *
       * Plus interface soft-drain can be issued only from one side of the link.
       * SPF should consider max metric of the bi-directional adj instead of
       * uni-directional one from "current node" to "other node".
       */
      auto metric = useLinkMetric ? link->getMaxMetric() : 1;
      auto otherNode = q.get(otherNodeName);
      if (!otherNode) {
        q.insertNode(otherNodeName, recordedNodeMetric + metric);
        otherNode = q.get(otherNodeName);
      }
      if (otherNode->metric() >= recordedNodeMetric + metric) {
        // recordedNodeName is either along an alternate shortest path towards
        // otherNodeName or is along a new shorter path. In either case,
        // otherNodeName should use recordedNodeName's nextHops until it finds
        // some shorter path
        if (otherNode->metric() > recordedNodeMetric + metric) {
          // if this is strictly better, forget about any other paths
          otherNode->result.reset(recordedNodeMetric + metric);
          q.reMake();
        }
        auto& otherNodeResult = otherNode->result;
        otherNodeResult.addPath(link, recordedNodeName);
        otherNodeResult.addNextHops(recordedNodeNextHops);
        if (otherNodeResult.nextHops().empty()) {
          // directly connected node
          otherNodeResult.addNextHop(otherNodeName);
        }
      }
    }
  }
  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  XLOG(DBG3) << "SPF elapsed time: " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue("decision.spf_ms", deltaTime.count(), fb303::AVG);
  return result;
}

} // namespace openr
