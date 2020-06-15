/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/decision/LinkState.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <fb303/ServiceData.h>
#include <folly/Format.h>
#include <openr/common/Util.h>

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

template <class T>
HoldableValue<T>::HoldableValue(T val) : val_(val) {}

template <class T>
void
HoldableValue<T>::operator=(T val) {
  val_ = val;
  heldVal_.reset();
  holdTtl_ = 0;
}

template <class T>
const T&
HoldableValue<T>::value() const {
  return heldVal_.has_value() ? heldVal_.value() : val_;
}

template <class T>
bool
HoldableValue<T>::hasHold() const {
  return heldVal_.has_value();
}

template <class T>
bool
HoldableValue<T>::decrementTtl() {
  if (heldVal_ && 0 == --holdTtl_) {
    heldVal_.reset();
    return true;
  }
  return false;
}

template <class T>
bool
HoldableValue<T>::updateValue(
    T val, LinkStateMetric holdUpTtl, LinkStateMetric holdDownTtl) {
  // calling update with the same value is a no-op
  if (val != val_) {
    if (hasHold()) {
      // If there was already a hold we need to fall back to fast update.
      // Otherwise, there are cases that could lead to longer transient
      // (less transient?) loops.
      heldVal_.reset();
      holdTtl_ = 0;
    } else {
      holdTtl_ = isChangeBringingUp(val) ? holdUpTtl : holdDownTtl;
      if (0 != holdTtl_) {
        heldVal_ = val_;
      }
    }
    val_ = val;
    return !hasHold();
  }
  return false;
}

template <>
bool
HoldableValue<bool>::isChangeBringingUp(bool val) {
  return val_ && !val;
}

template <>
bool
HoldableValue<LinkStateMetric>::isChangeBringingUp(LinkStateMetric val) {
  return val < val_;
}

// explicit instantiations for our use cases
template class HoldableValue<LinkStateMetric>;
template class HoldableValue<bool>;

Link::Link(
    const std::string& area,
    const std::string& nodeName1,
    const std::string& if1,
    const std::string& nodeName2,
    const std::string& if2)
    : area_(area),
      n1_(nodeName1),
      n2_(nodeName2),
      if1_(if1),
      if2_(if2),
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
    const openr::thrift::Adjacency& adj2)
    : Link(area, nodeName1, adj1.ifName, nodeName2, adj2.ifName) {
  metric1_ = adj1.metric;
  metric2_ = adj2.metric;
  overload1_ = adj1.isOverloaded;
  overload2_ = adj2.isOverloaded;
  adjLabel1_ = adj1.adjLabel;
  adjLabel2_ = adj2.adjLabel;
  nhV41_ = adj1.nextHopV4;
  nhV42_ = adj2.nextHopV4;
  nhV61_ = adj1.nextHopV6;
  nhV62_ = adj2.nextHopV6;
}

const std::string&
Link::getOtherNodeName(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return n2_;
  }
  if (n2_ == nodeName) {
    return n1_;
  }
  throw std::invalid_argument(nodeName);
}

const std::string&
Link::firstNodeName() const {
  return orderedNames_.first.first;
}

const std::string&
Link::secondNodeName() const {
  return orderedNames_.second.first;
}

const std::string&
Link::getIfaceFromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return if1_;
  }
  if (n2_ == nodeName) {
    return if2_;
  }
  throw std::invalid_argument(nodeName);
}

LinkStateMetric
Link::getMetricFromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return metric1_.value();
  }
  if (n2_ == nodeName) {
    return metric2_.value();
  }
  throw std::invalid_argument(nodeName);
}

int32_t
Link::getAdjLabelFromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return adjLabel1_;
  }
  if (n2_ == nodeName) {
    return adjLabel2_;
  }
  throw std::invalid_argument(nodeName);
}

bool
Link::getOverloadFromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return overload1_.value();
  }
  if (n2_ == nodeName) {
    return overload2_.value();
  }
  throw std::invalid_argument(nodeName);
}

void
Link::setHoldUpTtl(LinkStateMetric ttl) {
  holdUpTtl_ = ttl;
}

bool
Link::isUp() const {
  return (0 == holdUpTtl_) && !overload1_.value() && !overload2_.value();
}

bool
Link::decrementHolds() {
  bool holdExpired = false;
  if (0 != holdUpTtl_) {
    holdExpired |= (0 == --holdUpTtl_);
  }
  holdExpired |= metric1_.decrementTtl();
  holdExpired |= metric2_.decrementTtl();
  holdExpired |= overload1_.decrementTtl();
  holdExpired |= overload2_.decrementTtl();
  return holdExpired;
}

bool
Link::hasHolds() const {
  return 0 != holdUpTtl_ || metric1_.hasHold() || metric2_.hasHold() ||
      overload1_.hasHold() || overload2_.hasHold();
}

const thrift::BinaryAddress&
Link::getNhV4FromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return nhV41_;
  }
  if (n2_ == nodeName) {
    return nhV42_;
  }
  throw std::invalid_argument(nodeName);
}

const thrift::BinaryAddress&
Link::getNhV6FromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return nhV61_;
  }
  if (n2_ == nodeName) {
    return nhV62_;
  }
  throw std::invalid_argument(nodeName);
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
Link::setMetricFromNode(
    const std::string& nodeName,
    LinkStateMetric d,
    LinkStateMetric holdUpTtl,
    LinkStateMetric holdDownTtl) {
  if (n1_ == nodeName) {
    return metric1_.updateValue(d, holdUpTtl, holdDownTtl);
  } else if (n2_ == nodeName) {
    return metric2_.updateValue(d, holdUpTtl, holdDownTtl);
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

bool
Link::setOverloadFromNode(
    const std::string& nodeName,
    bool overload,
    LinkStateMetric holdUpTtl,
    LinkStateMetric holdDownTtl) {
  bool const wasUp = isUp();
  if (n1_ == nodeName) {
    overload1_.updateValue(overload, holdUpTtl, holdDownTtl);
  } else if (n2_ == nodeName) {
    overload2_.updateValue(overload, holdUpTtl, holdDownTtl);
  } else {
    throw std::invalid_argument(nodeName);
  }
  // since we don't support simplex overloads, we only signal topo change if
  // this is true
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
  return folly::sformat("{} - {}%{} <---> {}%{}", area_, n1_, if1_, n2_, if2_);
}

std::string
Link::directionalToString(const std::string& fromNode) const {
  return folly::sformat(
      "{} - {}%{} ---> {}%{}",
      area_,
      fromNode,
      getIfaceFromNode(fromNode),
      getOtherNodeName(fromNode),
      getIfaceFromNode(getOtherNodeName(fromNode)));
}

LinkState::LinkState(const std::string& area) : area_(area) {}

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
    LinkSet const& linksToIgnore) {
  if (src == dest) {
    return LinkState::Path{};
  }
  auto const& nodeResult = result.at(dest);
  for (auto const& pathLink : nodeResult.pathLinks()) {
    if (!linksToIgnore.count(pathLink.link)) {
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
    } catch (std::out_of_range const& e) {
      LOG(FATAL) << "std::out_of_range for " << nodeName;
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
    const std::string& nodeName,
    bool isOverloaded,
    LinkStateMetric holdUpTtl,
    LinkStateMetric holdDownTtl) {
  if (nodeOverloads_.count(nodeName)) {
    return nodeOverloads_.at(nodeName).updateValue(
        isOverloaded, holdUpTtl, holdDownTtl);
  }
  nodeOverloads_.emplace(nodeName, HoldableValue<bool>{isOverloaded});
  // don't indicate LinkState changed if this is a new node
  return false;
}

bool
LinkState::isNodeOverloaded(const std::string& nodeName) const {
  return nodeOverloads_.count(nodeName) && nodeOverloads_.at(nodeName).value();
}

bool
LinkState::decrementHolds() {
  bool holdChange = false;
  for (auto& link : allLinks_) {
    holdChange |= link->decrementHolds();
  }
  for (auto& kv : nodeOverloads_) {
    holdChange |= kv.second.decrementTtl();
  }
  if (holdChange) {
    spfResults_.clear();
    kthPathResults_.clear();
  }
  return holdChange;
}

bool
LinkState::hasHolds() const {
  for (auto& link : allLinks_) {
    if (link->hasHolds()) {
      return true;
    }
  }
  for (auto& kv : nodeOverloads_) {
    if (kv.second.hasHold()) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<Link>
LinkState::maybeMakeLink(
    const std::string& nodeName, const thrift::Adjacency& adj) const {
  // only return Link if it is bidirectional.
  auto search = adjacencyDatabases_.find(adj.otherNodeName);
  if (search != adjacencyDatabases_.end()) {
    for (const auto& otherAdj : search->second.adjacencies) {
      if (nodeName == otherAdj.otherNodeName &&
          adj.otherIfName == otherAdj.ifName &&
          adj.ifName == otherAdj.otherIfName) {
        return std::make_shared<Link>(
            area_, nodeName, adj, adj.otherNodeName, otherAdj);
      }
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<Link>>
LinkState::getOrderedLinkSet(const thrift::AdjacencyDatabase& adjDb) const {
  std::vector<std::shared_ptr<Link>> links;
  links.reserve(adjDb.adjacencies.size());
  for (const auto& adj : adjDb.adjacencies) {
    auto linkPtr = maybeMakeLink(adjDb.thisNodeName, adj);
    if (nullptr != linkPtr) {
      links.emplace_back(linkPtr);
    }
  }
  links.shrink_to_fit();
  std::sort(links.begin(), links.end(), LinkState::LinkPtrLess{});
  return links;
}

std::pair<
    bool /* topology has changed*/,
    bool /* route attributes has changed (nexthop addr, node/adj label */>
LinkState::updateAdjacencyDatabase(
    thrift::AdjacencyDatabase const& newAdjacencyDb,
    LinkStateMetric holdUpTtl,
    LinkStateMetric holdDownTtl) {
  auto const& nodeName = newAdjacencyDb.thisNodeName;
  VLOG(1) << "Updating adjacency database for node " << nodeName << ", area "
          << newAdjacencyDb.area_ref().value_or("N/A");

  // Area field must be specified and match with area_
  DCHECK(newAdjacencyDb.area_ref());
  DCHECK_EQ(area_, newAdjacencyDb.area_ref().value());

  for (auto const& adj : newAdjacencyDb.adjacencies) {
    VLOG(3) << "  neighbor: " << adj.otherNodeName
            << ", remoteIfName: " << getRemoteIfName(adj)
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
  auto oldLinks = orderedLinksFromNode(nodeName);
  auto newLinks = getOrderedLinkSet(newAdjacencyDb);

  // fill these sets with the appropriate links
  std::unordered_set<Link> linksUp;
  std::unordered_set<Link> linksDown;

  bool topoChanged = updateNodeOverloaded(
      nodeName, newAdjacencyDb.isOverloaded, holdUpTtl, holdDownTtl);

  bool routeAttrChanged = false;

  routeAttrChanged |= priorAdjacencyDb.nodeLabel != newAdjacencyDb.nodeLabel;

  auto newIter = newLinks.begin();
  auto oldIter = oldLinks.begin();
  while (newIter != newLinks.end() || oldIter != oldLinks.end()) {
    if (newIter != newLinks.end() &&
        (oldIter == oldLinks.end() || **newIter < **oldIter)) {
      // newIter is pointing at a Link not currently present, record this as a
      // link to add and advance newIter
      (*newIter)->setHoldUpTtl(holdUpTtl);
      topoChanged |= (*newIter)->isUp();
      // even if we are holding a change, we apply the change to our link state
      // and check for holds when running spf. this ensures we don't add the
      // same hold twice
      addLink(*newIter);
      VLOG(1) << "addLink " << (*newIter)->toString();
      ++newIter;
      continue;
    }
    if (oldIter != oldLinks.end() &&
        (newIter == newLinks.end() || **oldIter < **newIter)) {
      // oldIter is pointing at a Link that is no longer present, record this
      // as a link to remove and advance oldIter.
      // If this link was previously overloaded or had a hold up, this does not
      // change the topology.
      topoChanged |= (*oldIter)->isUp();
      removeLink(*oldIter);
      VLOG(1) << "removeLink " << (*oldIter)->toString();
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
      LOG(INFO) << folly::sformat(
          "Metric change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          oldLink.getMetricFromNode(nodeName),
          newLink.getMetricFromNode(nodeName));
      topoChanged = oldLink.setMetricFromNode(
          nodeName,
          newLink.getMetricFromNode(nodeName),
          holdUpTtl,
          holdDownTtl);
    }

    if (newLink.getOverloadFromNode(nodeName) !=
        oldLink.getOverloadFromNode(nodeName)) {
      LOG(INFO) << folly::sformat(
          "Overload change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          oldLink.getOverloadFromNode(nodeName),
          newLink.getOverloadFromNode(nodeName));
      topoChanged = oldLink.setOverloadFromNode(
          nodeName,
          newLink.getOverloadFromNode(nodeName),
          holdUpTtl,
          holdDownTtl);
    }

    // Check if adjacency label has changed
    if (newLink.getAdjLabelFromNode(nodeName) !=
        oldLink.getAdjLabelFromNode(nodeName)) {
      VLOG(1) << folly::sformat(
          "AdjLabel change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          oldLink.getAdjLabelFromNode(nodeName),
          newLink.getAdjLabelFromNode(nodeName));

      // Route attribute changes only when adjLabel has changed for local node
      routeAttrChanged |= true;

      // change the adjLabel on the link object we already have
      oldLink.setAdjLabelFromNode(
          nodeName, newLink.getAdjLabelFromNode(nodeName));
    }

    // check if local nextHops Changed
    if (newLink.getNhV4FromNode(nodeName) !=
        oldLink.getNhV4FromNode(nodeName)) {
      VLOG(1) << folly::sformat(
          "V4-NextHop address change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          toString(oldLink.getNhV4FromNode(nodeName)),
          toString(newLink.getNhV4FromNode(nodeName)));

      routeAttrChanged |= true;
      oldLink.setNhV4FromNode(nodeName, newLink.getNhV4FromNode(nodeName));
    }
    if (newLink.getNhV6FromNode(nodeName) !=
        oldLink.getNhV6FromNode(nodeName)) {
      VLOG(1) << folly::sformat(
          "V4-NextHop address change on link {}: {} => {}",
          newLink.directionalToString(nodeName),
          toString(oldLink.getNhV6FromNode(nodeName)),
          toString(newLink.getNhV6FromNode(nodeName)));

      routeAttrChanged |= true;
      oldLink.setNhV6FromNode(nodeName, newLink.getNhV6FromNode(nodeName));
    }
    ++newIter;
    ++oldIter;
  }
  if (topoChanged) {
    spfResults_.clear();
    kthPathResults_.clear();
  }
  return std::make_pair(topoChanged, routeAttrChanged);
}

bool
LinkState::deleteAdjacencyDatabase(const std::string& nodeName) {
  VLOG(1) << "Deleting adjacency database for node " << nodeName;
  auto search = adjacencyDatabases_.find(nodeName);

  if (search == adjacencyDatabases_.end()) {
    LOG(WARNING) << "Trying to delete adjacency db for nonexisting node "
                 << nodeName;
    return false;
  }
  removeNode(nodeName);
  adjacencyDatabases_.erase(search);
  spfResults_.clear();
  kthPathResults_.clear();
  return true;
}

LinkStateMetric
LinkState::getHopsFromAToB(std::string const& a, std::string const& b) {
  if (a == b) {
    return 0;
  }
  auto const& spfResult = getSpfResult(a, false);
  if (spfResult.count(b)) {
    return spfResult.at(b).metric();
  }
  return getMaxHopsToNode(b);
}

LinkStateMetric
LinkState::getMaxHopsToNode(const std::string& nodeName) {
  LinkStateMetric max = 0;
  for (auto const& pathsFromNode : getSpfResult(nodeName, false)) {
    max = std::max(max, pathsFromNode.second.metric());
  }
  return max;
}

std::vector<LinkState::Path> const&
LinkState::getKthPaths(
    const std::string& src, const std::string& dest, size_t k) {
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
        for (auto const& link : *path) {
          visitedLinks.insert(link);
        }
        paths.push_back(std::move(*path));
        path = traceOnePath(src, dest, res, visitedLinks);
      }
    }
    entryIter = kthPathResults_.emplace(key, std::move(paths)).first;
  }
  return entryIter->second;
}

LinkState::SpfResult const&
LinkState::getSpfResult(const std::string& thisNodeName, bool useLinkMetric) {
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
    const LinkState::LinkSet& linksToIgnore) {
  LinkState::SpfResult result;

  fb303::fbData->addStatValue("decision.spf_runs", 1, fb303::COUNT);
  const auto startTime = std::chrono::steady_clock::now();

  DijkstraQ q;
  q.insertNode(thisNodeName, 0);
  uint64_t loop = 0;
  while (auto node = q.extractMin()) {
    ++loop;
    // we've found this node's shortest paths. record it
    auto emplaceRc = result.emplace(node->nodeName, std::move(node->result));
    CHECK(emplaceRc.second);

    auto const& recordedNodeName = emplaceRc.first->first;
    auto const recordedNodeMetric = emplaceRc.first->second.metric();
    auto const& recordedNodeNextHops = emplaceRc.first->second.nextHops();

    if (isNodeOverloaded(recordedNodeName) &&
        recordedNodeName != thisNodeName) {
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
    // this is the "relax" step in the Dijkstra Algorithm pseudocode in CLRS
    for (const auto& link : linksFromNode(recordedNodeName)) {
      auto& otherNodeName = link->getOtherNodeName(recordedNodeName);
      if (!link->isUp() or result.count(otherNodeName) or
          linksToIgnore.count(link)) {
        continue;
      }
      auto metric =
          useLinkMetric ? link->getMetricFromNode(recordedNodeName) : 1;
      auto otherNode = q.get(otherNodeName);
      if (!otherNode) {
        q.insertNode(otherNodeName, recordedNodeMetric + metric);
        otherNode = q.get(otherNodeName);
      }
      if (otherNode->result.metric() >= recordedNodeMetric + metric) {
        // recordedNodeName is either along an alternate shortest path towards
        // otherNodeName or is along a new shorter path. In either case,
        // otherNodeName should use recordedNodeName's nextHops until it finds
        // some shorter path
        if (otherNode->result.metric() > recordedNodeMetric + metric) {
          // if this is strictly better, forget about any other paths
          otherNode->result.reset(recordedNodeMetric + metric);
          q.reMake();
        }
        otherNode->result.addPath(link, recordedNodeName);
        otherNode->result.addNextHops(recordedNodeNextHops);
        if (otherNode->result.nextHops().empty()) {
          // directly connected node
          otherNode->result.addNextHop(otherNodeName);
        }
      }
    }
  }
  VLOG(3) << "Dijkstra loop count: " << loop;
  auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startTime);
  LOG(INFO) << "SPF elapsed time: " << deltaTime.count() << "ms.";
  fb303::fbData->addStatValue("decision.spf_ms", deltaTime.count(), fb303::AVG);
  return result;
}

} // namespace openr
