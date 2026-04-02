/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/decision/Link.h>

size_t
std::hash<openr::Link>::operator()(openr::Link const& link) const {
  return link.hash;
}

bool
std::equal_to<openr::Link::LinkSet>::operator()(
    openr::Link::LinkSet const& a, openr::Link::LinkSet const& b) const {
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
std::hash<openr::Link::LinkSet>::operator()(
    openr::Link::LinkSet const& set) const {
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
      hash(
          std::hash<std::pair<
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

bool
Link::isUp() const {
  return (!overload1_) && (!overload2_) && usable_;
}

const std::string&
Link::getArea() const {
  return area_;
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
    return metric1_;
  }
  if (n2_ == nodeName) {
    return metric2_;
  }
  throw std::invalid_argument(nodeName);
}

LinkStateMetric
Link::getMaxMetric() const {
  return std::max(metric1_, metric2_);
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

int64_t
Link::getWeightFromNode(const std::string& nodeName) const {
  if (n1_ == nodeName) {
    return weight1_;
  }
  if (n2_ == nodeName) {
    return weight2_;
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

bool
Link::getUsability() const {
  return usable_;
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

size_t
Link::LinkPtrHash::operator()(const std::shared_ptr<Link>& l) const {
  return l->hash;
}

bool
Link::LinkPtrLess::operator()(
    const std::shared_ptr<Link>& lhs, const std::shared_ptr<Link>& rhs) const {
  return *lhs < *rhs;
}

bool
Link::LinkPtrEqual::operator()(
    const std::shared_ptr<Link>& lhs, const std::shared_ptr<Link>& rhs) const {
  return *lhs == *rhs;
}

} // namespace openr
