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

#include <folly/Format.h>

size_t
std::hash<openr::Link>::operator()(openr::Link const& link) const {
  return link.hash;
}

namespace openr {

Link::Link(
    const std::string& nodeName1,
    const openr::thrift::Adjacency& adj1,
    const std::string& nodeName2,
    const openr::thrift::Adjacency& adj2)
    : n1_(nodeName1),
      n2_(nodeName2),
      if1_(adj1.ifName),
      if2_(adj2.ifName),
      metric1_(adj1.metric),
      metric2_(adj2.metric),
      adjLabel1_(adj1.adjLabel),
      adjLabel2_(adj2.adjLabel),
      overload1_(adj1.isOverloaded),
      overload2_(adj2.isOverloaded),
      nhV41_(adj1.nextHopV4),
      nhV42_(adj2.nextHopV4),
      nhV61_(adj1.nextHopV6),
      nhV62_(adj2.nextHopV6),
      orderedNames(
          std::minmax(std::make_pair(n1_, if1_), std::make_pair(n2_, if2_))),
      hash(std::hash<std::pair<
               std::pair<std::string, std::string>,
               std::pair<std::string, std::string>>>()(orderedNames)) {}

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
  return orderedNames.first.first;
}

const std::string&
Link::secondNodeName() const {
  return orderedNames.second.first;
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
    return metric1_;
  }
  if (n2_ == nodeName) {
    return metric2_;
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
    return overload1_;
  }
  if (n2_ == nodeName) {
    return overload2_;
  }
  throw std::invalid_argument(nodeName);
}

bool
Link::isOverloaded() const {
  return overload1_ || overload2_;
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

void
Link::setMetricFromNode(const std::string& nodeName, LinkStateMetric d) {
  if (n1_ == nodeName) {
    metric1_ = d;
  } else if (n2_ == nodeName) {
    metric2_ = d;
  } else {
    throw std::invalid_argument(nodeName);
  }
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
Link::setOverloadFromNode(const std::string& nodeName, bool overload) {
  if (n1_ == nodeName) {
    overload1_ = overload;
  } else if (n2_ == nodeName) {
    overload2_ = overload;
  } else {
    throw std::invalid_argument(nodeName);
  }
}

bool
Link::operator<(const Link& other) const {
  if (this->hash != other.hash) {
    return this->hash < other.hash;
  }
  return this->orderedNames < other.orderedNames;
}

bool
Link::operator==(const Link& other) const {
  if (this->hash != other.hash) {
    return false;
  }
  return this->orderedNames == other.orderedNames;
}

std::string
Link::toString() const {
  return folly::sformat("{}%{} <---> {}%{}", n1_, if1_, n2_, if2_);
}

std::string
Link::directionalToString(const std::string& fromNode) const {
  return folly::sformat(
      "{}%{} ---> {}%{}",
      fromNode,
      getIfaceFromNode(fromNode),
      getOtherNodeName(fromNode),
      getIfaceFromNode(getOtherNodeName(fromNode)));
}

bool
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

void
LinkState::addLink(std::shared_ptr<Link> link) {
  CHECK(linkMap_[link->firstNodeName()].insert(link).second);
  CHECK(linkMap_[link->secondNodeName()].insert(link).second);
}

// throws std::out_of_range if links are not present
void
LinkState::removeLink(std::shared_ptr<Link> link) {
  CHECK(linkMap_.at(link->firstNodeName()).erase(link));
  CHECK(linkMap_.at(link->secondNodeName()).erase(link));
}

void
LinkState::removeLinksFromNode(const std::string& nodeName) {
  auto search = linkMap_.find(nodeName);
  if (search == linkMap_.end()) {
    // No links were added (addition of empty adjacency db can cause this)
    return;
  }

  // erase ptrs to these links from other nodes
  for (auto const& link : search->second) {
    try {
      CHECK(linkMap_.at(link->getOtherNodeName(nodeName)).erase(link));
    } catch (std::out_of_range const& e) {
      LOG(FATAL) << "std::out_of_range for " << nodeName;
    }
  }
  linkMap_.erase(search);
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
LinkState::orderedLinksFromNode(const std::string& nodeName) {
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
  // don't indicate LinkState changed if this is a new node, only if it causes
  // some new links to come up
  bool changed = nodeOverloads_.count(nodeName) &&
      (isOverloaded != nodeOverloads_.at(nodeName));
  nodeOverloads_[nodeName] = isOverloaded;
  return changed;
}

bool
LinkState::isNodeOverloaded(const std::string& nodeName) const {
  return nodeOverloads_.count(nodeName) && nodeOverloads_.at(nodeName);
}

} // namespace openr
