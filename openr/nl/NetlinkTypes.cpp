/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <net/if.h>
}

namespace openr::fbnl {

/*=================================Route====================================*/

Route
RouteBuilder::build() const {
  return Route(*this);
}

RouteBuilder&
RouteBuilder::setDestination(const folly::CIDRNetwork& dst) {
  dst_ = dst;
  family_ = std::get<0>(dst).family();
  return *this;
}

const folly::CIDRNetwork&
RouteBuilder::getDestination() const {
  return dst_;
}

RouteBuilder&
RouteBuilder::setMplsLabel(uint32_t mplsLabel) {
  mplsLabel_ = mplsLabel;
  family_ = AF_MPLS;
  return *this;
}

std::optional<uint32_t>
RouteBuilder::getMplsLabel() const {
  return mplsLabel_;
}

RouteBuilder&
RouteBuilder::setType(uint8_t type) {
  type_ = type;
  return *this;
}

uint8_t
RouteBuilder::getType() const {
  return type_;
}

RouteBuilder&
RouteBuilder::setRouteTable(uint32_t routeTable) {
  routeTable_ = routeTable;
  return *this;
}

uint32_t
RouteBuilder::getRouteTable() const {
  return routeTable_;
}

RouteBuilder&
RouteBuilder::setProtocolId(uint8_t protocolId) {
  protocolId_ = protocolId;
  return *this;
}

uint8_t
RouteBuilder::getProtocolId() const {
  return protocolId_;
}

RouteBuilder&
RouteBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

uint8_t
RouteBuilder::getScope() const {
  return scope_;
}

// Optional parameters set after object is constructed
RouteBuilder&
RouteBuilder::setFlags(uint32_t flags) {
  flags_ = flags;
  return *this;
}

std::optional<uint32_t>
RouteBuilder::getFlags() const {
  return flags_;
}

RouteBuilder&
RouteBuilder::setPriority(uint32_t priority) {
  priority_ = priority;
  return *this;
}

std::optional<uint32_t>
RouteBuilder::getPriority() const {
  return priority_;
}

RouteBuilder&
RouteBuilder::setTos(uint8_t tos) {
  tos_ = tos;
  return *this;
}

std::optional<uint8_t>
RouteBuilder::getTos() const {
  return tos_;
}

RouteBuilder&
RouteBuilder::setMtu(uint32_t mtu) {
  mtu_ = mtu;
  return *this;
}

std::optional<uint32_t>
RouteBuilder::getMtu() const {
  return mtu_;
}

RouteBuilder&
RouteBuilder::setAdvMss(uint32_t advMss) {
  advMss_ = advMss;
  return *this;
}

std::optional<uint32_t>
RouteBuilder::getAdvMss() const {
  return advMss_;
}

RouteBuilder&
RouteBuilder::addNextHop(const NextHop& nextHop) {
  nextHops_.emplace(nextHop);
  return *this;
}

const NextHopSet&
RouteBuilder::getNextHops() const {
  return nextHops_;
}

uint8_t
RouteBuilder::getFamily() const {
  return family_;
}

RouteBuilder&
RouteBuilder::setValid(bool isValid) {
  isValid_ = isValid;
  return *this;
}

bool
RouteBuilder::isValid() const {
  return isValid_;
}

RouteBuilder&
RouteBuilder::setMultiPath(bool isMultiPath) {
  isMultiPath_ = isMultiPath;
  return *this;
}

bool
RouteBuilder::isMultiPath() const {
  return isMultiPath_;
}

void
RouteBuilder::reset() {
  type_ = RTN_UNICAST;
  routeTable_ = RT_TABLE_MAIN;
  protocolId_ = DEFAULT_PROTOCOL_ID;
  scope_ = RT_SCOPE_UNIVERSE;
  isValid_ = false;
  flags_.reset();
  priority_.reset();
  tos_.reset();
  mtu_.reset();
  advMss_.reset();
  nextHops_.clear();
  isMultiPath_ = true;
}

Route::Route(const RouteBuilder& builder)
    : type_(builder.getType()),
      routeTable_(builder.getRouteTable()),
      protocolId_(builder.getProtocolId()),
      scope_(builder.getScope()),
      family_(builder.getFamily()),
      isValid_(builder.isValid()),
      flags_(builder.getFlags()),
      priority_(builder.getPriority()),
      tos_(builder.getTos()),
      mtu_(builder.getMtu()),
      advMss_(builder.getAdvMss()),
      nextHops_(builder.getNextHops()),
      dst_(builder.getDestination()),
      mplsLabel_(builder.getMplsLabel()),
      isMultiPath_(builder.isMultiPath()) {}

Route::~Route() {}

Route::Route(Route&& other) noexcept {
  *this = std::move(other);
}

Route&
Route::operator=(Route&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  type_ = std::move(other.type_);
  routeTable_ = std::move(other.routeTable_);
  protocolId_ = std::move(other.protocolId_);
  scope_ = std::move(other.scope_);
  isValid_ = std::move(other.isValid_);
  flags_ = std::move(other.flags_);
  priority_ = std::move(other.priority_);
  tos_ = std::move(other.tos_);
  mtu_ = std::move(other.mtu_);
  advMss_ = std::move(other.advMss_);
  nextHops_ = std::move(other.nextHops_);
  dst_ = std::move(other.dst_);
  family_ = std::move(other.family_);
  mplsLabel_ = std::move(other.mplsLabel_);
  isMultiPath_ = std::move(other.isMultiPath_);
  return *this;
}

Route::Route(const Route& other) {
  *this = other;
}

Route&
Route::operator=(const Route& other) {
  if (this == &other) {
    return *this;
  }
  type_ = other.type_;
  routeTable_ = other.routeTable_;
  protocolId_ = other.protocolId_;
  scope_ = other.scope_;
  isValid_ = other.isValid_;
  flags_ = other.flags_;
  priority_ = other.priority_;
  tos_ = other.tos_;
  mtu_ = other.mtu_;
  advMss_ = other.advMss_;
  nextHops_ = other.nextHops_;
  dst_ = other.dst_;
  family_ = other.family_;
  mplsLabel_ = other.mplsLabel_;
  isMultiPath_ = other.isMultiPath_;
  return *this;
}

bool
operator==(const Route& lhs, const Route& rhs) {
  bool ret =
      (lhs.getDestination() == rhs.getDestination() &&
       lhs.getMplsLabel() == rhs.getMplsLabel() &&
       lhs.getNextHops().size() == rhs.getNextHops().size() &&
       lhs.getType() == rhs.getType() &&
       lhs.getRouteTable() == rhs.getRouteTable() &&
       lhs.getProtocolId() == rhs.getProtocolId() &&
       lhs.getScope() == rhs.getScope() && lhs.isValid() == rhs.isValid() &&
       lhs.getFlags() == rhs.getFlags() &&
       lhs.getPriority() == rhs.getPriority() && lhs.getTos() == rhs.getTos() &&
       lhs.getMtu() == rhs.getMtu() && lhs.getAdvMss() == rhs.getAdvMss() &&
       lhs.getFamily() == rhs.getFamily());

  if (!ret) {
    return false;
  }

  // Verify all nexthops are in each other (NOTE: size of nexthops are same)
  for (const NextHop& nh : lhs.getNextHops()) {
    if (!rhs.getNextHops().count(nh)) {
      return false;
    }
  }

  return true;
}

uint8_t
Route::getFamily() const {
  return family_;
}

uint8_t
Route::getType() const {
  return type_;
}

const folly::CIDRNetwork&
Route::getDestination() const {
  return dst_;
}

std::optional<uint32_t>
Route::getMplsLabel() const {
  return mplsLabel_;
}

std::optional<uint8_t>
Route::getTos() const {
  return tos_;
}

std::optional<uint32_t>
Route::getMtu() const {
  return mtu_;
}

std::optional<uint32_t>
Route::getAdvMss() const {
  return advMss_;
}

uint32_t
Route::getRouteTable() const {
  return routeTable_;
}

uint8_t
Route::getProtocolId() const {
  return protocolId_;
}

uint8_t
Route::getScope() const {
  return scope_;
}

std::optional<uint32_t>
Route::getFlags() const {
  return flags_;
}

std::optional<uint32_t>
Route::getPriority() const {
  return priority_;
}

const NextHopSet&
Route::getNextHops() const {
  return nextHops_;
}

bool
Route::isValid() const {
  return isValid_;
}

bool
Route::isMultiPath() const {
  return isMultiPath_;
}

std::string
Route::str() const {
  std::string result;
  if (family_ == AF_MPLS) {
    if (mplsLabel_.has_value()) {
      result += fmt::format("label {} ", mplsLabel_.value());
    }
  } else {
    result += fmt::format("route {} ", folly::IPAddress::networkToString(dst_));
  }
  result += fmt::format(
      "proto {}, table {}, valid {}, family {}, flags {}, scope {}, type {}",
      protocolId_,
      routeTable_,
      isValid_ ? "Yes" : "No",
      static_cast<int>(family_),
      flags_.has_value() ? flags_.value() : 0,
      scope_,
      static_cast<int>(type_));

  if (priority_) {
    result += fmt::format(", priority {}", priority_.value());
  }
  if (tos_) {
    result += fmt::format(", tos {}", tos_.value());
  }
  if (mtu_) {
    result += fmt::format(", mtu {}", mtu_.value());
  }
  if (advMss_) {
    result += fmt::format(", advmss {}", advMss_.value());
  }
  for (auto const& nextHop : nextHops_) {
    result += "\n  " + nextHop.str();
  }
  return result;
}

void
Route::setPriority(uint32_t priority) {
  priority_ = priority;
}

void
Route::setNextHops(const NextHopSet& nextHops) {
  nextHops_ = nextHops;
}

/*=================================NextHop====================================*/

NextHop
NextHopBuilder::build() const {
  return NextHop(*this);
}

void
NextHopBuilder::reset() {
  ifIndex_.reset();
  weight_ = 0;
  gateway_.reset();
  labelAction_.reset();
  swapLabel_.reset();
  pushLabels_.reset();
  family_.reset();
}

NextHopBuilder&
NextHopBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setGateway(const folly::IPAddress& gateway) {
  gateway_ = gateway;
  return *this;
}

NextHopBuilder&
NextHopBuilder::unsetGateway() {
  gateway_.reset();
  return *this;
}

NextHopBuilder&
NextHopBuilder::setWeight(uint8_t weight) {
  weight_ = weight;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setLabelAction(thrift::MplsActionCode action) {
  labelAction_ = action;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setSwapLabel(uint32_t swapLabel) {
  swapLabel_ = swapLabel;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setPushLabels(const std::vector<int32_t>& pushLabels) {
  pushLabels_ = pushLabels;
  return *this;
}

std::optional<int>
NextHopBuilder::getIfIndex() const {
  return ifIndex_;
}

std::optional<folly::IPAddress>
NextHopBuilder::getGateway() const {
  return gateway_;
}

uint8_t
NextHopBuilder::getWeight() const {
  return weight_;
}

std::optional<thrift::MplsActionCode>
NextHopBuilder::getLabelAction() const {
  return labelAction_;
}

std::optional<uint32_t>
NextHopBuilder::getSwapLabel() const {
  return swapLabel_;
}

std::optional<std::vector<int32_t>>
NextHopBuilder::getPushLabels() const {
  return pushLabels_;
}

uint8_t
NextHopBuilder::getFamily() const {
  if (gateway_.has_value()) {
    return gateway_.value().family();
  }
  return AF_UNSPEC;
}

NextHop::NextHop(const NextHopBuilder& builder)
    : ifIndex_(builder.getIfIndex()),
      gateway_(builder.getGateway()),
      weight_(builder.getWeight()),
      labelAction_(builder.getLabelAction()),
      swapLabel_(builder.getSwapLabel()),
      pushLabels_(builder.getPushLabels()),
      family_(builder.getFamily()) {}

bool
operator==(const NextHop& lhs, const NextHop& rhs) {
  return lhs.getIfIndex() == rhs.getIfIndex() &&
      lhs.getGateway() == rhs.getGateway() &&
      std::max(lhs.getWeight(), uint8_t(1)) ==
      std::max(rhs.getWeight(), uint8_t(1)) &&
      lhs.getLabelAction() == rhs.getLabelAction() &&
      lhs.getSwapLabel() == rhs.getSwapLabel() &&
      lhs.getPushLabels() == rhs.getPushLabels() &&
      lhs.getFamily() == rhs.getFamily();
}

size_t
NextHopHash::operator()(const NextHop& nh) const {
  size_t res = 0;
  if (nh.getIfIndex().has_value()) {
    res += std::hash<std::string>()(std::to_string(nh.getIfIndex().value()));
  }
  if (nh.getGateway().has_value()) {
    res += std::hash<std::string>()(nh.getGateway().value().str());
  }
  res += std::hash<std::string>()(
      std::to_string(std::max(nh.getWeight(), uint8_t(1))));
  return res;
}

std::optional<int>
NextHop::getIfIndex() const {
  return ifIndex_;
}

std::optional<folly::IPAddress>
NextHop::getGateway() const {
  return gateway_;
}

uint8_t
NextHop::getWeight() const {
  return weight_;
}

std::optional<thrift::MplsActionCode>
NextHop::getLabelAction() const {
  return labelAction_;
}

std::optional<uint32_t>
NextHop::getSwapLabel() const {
  return swapLabel_;
}

std::optional<std::vector<int32_t>>
NextHop::getPushLabels() const {
  return pushLabels_;
}

void
NextHop::setPushLabels(std::vector<int32_t> pushLabels) {
  pushLabels_ = pushLabels;
}

uint8_t
NextHop::getFamily() const {
  if (gateway_.has_value()) {
    return gateway_.value().family();
  }
  return AF_UNSPEC;
}

std::string
NextHop::str() const {
  std::string result;
  result += fmt::format(
      "nexthop via {}, intf-index {}, weight {}, family {}",
      (gateway_ ? gateway_->str() : "n/a"),
      (ifIndex_ ? std::to_string(*ifIndex_) : "n/a"),
      std::to_string(weight_),
      static_cast<int>(getFamily()));
  if (labelAction_.has_value()) {
    result += fmt::format(
        " Label action {}",
        apache::thrift::util::enumNameSafe(labelAction_.value()));
  }
  if (swapLabel_.has_value()) {
    result += fmt::format(" Swap label {}", swapLabel_.value());
  }
  if (pushLabels_.has_value()) {
    result += " Push Labels: ";
    for (const auto& label : pushLabels_.value()) {
      result += fmt::format(" {} ", label);
    }
  }
  return result;
}

/*================================IfAddress===================================*/

IfAddress
IfAddressBuilder::build() const {
  return IfAddress(*this);
}

IfAddressBuilder&
IfAddressBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int
IfAddressBuilder::getIfIndex() const {
  return ifIndex_;
}

IfAddressBuilder&
IfAddressBuilder::setPrefix(const folly::CIDRNetwork& prefix) {
  prefix_ = prefix;
  return *this;
}

std::optional<folly::CIDRNetwork>
IfAddressBuilder::getPrefix() const {
  return prefix_;
}

IfAddressBuilder&
IfAddressBuilder::setFamily(uint8_t family) {
  family_ = family;
  return *this;
}

// Family will be shadowed if prefix is set
std::optional<uint8_t>
IfAddressBuilder::getFamily() const {
  return family_;
}

IfAddressBuilder&
IfAddressBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

std::optional<uint8_t>
IfAddressBuilder::getScope() const {
  return scope_;
}

IfAddressBuilder&
IfAddressBuilder::setFlags(uint8_t flags) {
  flags_ = flags;
  return *this;
}

std::optional<uint8_t>
IfAddressBuilder::getFlags() const {
  return flags_;
}

IfAddressBuilder&
IfAddressBuilder::setValid(bool isValid) {
  isValid_ = isValid;
  return *this;
}

bool
IfAddressBuilder::isValid() const {
  return isValid_;
}

void
IfAddressBuilder::reset() {
  ifIndex_ = 0;
  isValid_ = false;
  prefix_.reset();
  scope_.reset();
  flags_.reset();
  family_.reset();
}

IfAddress::IfAddress(const IfAddressBuilder& builder)
    : prefix_(builder.getPrefix()),
      ifIndex_(builder.getIfIndex()),
      isValid_(builder.isValid()),
      scope_(builder.getScope()),
      flags_(builder.getFlags()),
      family_(builder.getFamily()) {}

IfAddress::~IfAddress() {}

IfAddress::IfAddress(IfAddress&& other) noexcept {
  *this = std::move(other);
}

IfAddress&
IfAddress::operator=(IfAddress&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  prefix_ = std::move(other.prefix_);
  ifIndex_ = std::move(other.ifIndex_);
  isValid_ = std::move(other.isValid_);
  scope_ = std::move(other.scope_);
  flags_ = std::move(other.flags_);
  family_ = std::move(other.family_);
  return *this;
}

IfAddress::IfAddress(const IfAddress& other) {
  *this = other;
}

IfAddress&
IfAddress::operator=(const IfAddress& other) {
  if (this == &other) {
    return *this;
  }

  prefix_ = other.prefix_;
  ifIndex_ = other.ifIndex_;
  isValid_ = other.isValid_;
  scope_ = other.scope_;
  flags_ = other.flags_;
  family_ = other.family_;

  return *this;
}

uint8_t
IfAddress::getFamily() const {
  if (prefix_.has_value()) {
    return prefix_->first.family();
  } else {
    return family_.value();
  }
}

uint8_t
IfAddress::getPrefixLen() const {
  if (prefix_.has_value()) {
    return prefix_->second;
  }
  return 0;
}

int
IfAddress::getIfIndex() const {
  return ifIndex_;
}

bool
IfAddress::isValid() const {
  return isValid_;
}

std::optional<folly::CIDRNetwork>
IfAddress::getPrefix() const {
  return prefix_;
}

std::optional<uint8_t>
IfAddress::getScope() const {
  return scope_;
}

std::optional<uint8_t>
IfAddress::getFlags() const {
  return flags_;
}

std::string
IfAddress::str() const {
  return fmt::format(
      "addr {} {} intf-index {}, valid {}, scope {}",
      getFamily() == AF_INET ? "inet" : "inet6",
      prefix_.has_value() ? folly::IPAddress::networkToString(*prefix_) : "n/a",
      ifIndex_,
      isValid_ ? "Yes" : "No",
      scope_.has_value() ? scope_.value() : -1);
}

bool
operator==(const IfAddress& lhs, const IfAddress& rhs) {
  return (
      lhs.getPrefix() == rhs.getPrefix() &&
      lhs.getIfIndex() == rhs.getIfIndex() && lhs.isValid() == rhs.isValid() &&
      lhs.getScope() == rhs.getScope() && lhs.getFlags() == rhs.getFlags() &&
      lhs.getFamily() == rhs.getFamily());
}

/*================================Neighbor====================================*/

bool
isNeighborReachable(int state) {
  return state == NUD_REACHABLE or state == NUD_STALE or state == NUD_DELAY or
      state == NUD_PERMANENT or state == NUD_PROBE or state == NUD_NOARP;
}

Neighbor
NeighborBuilder::build() const {
  return Neighbor(*this);
}

NeighborBuilder&
NeighborBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int
NeighborBuilder::getIfIndex() const {
  return ifIndex_;
}

NeighborBuilder&
NeighborBuilder::setDestination(const folly::IPAddress& dest) {
  destination_ = dest;
  return *this;
}

folly::IPAddress
NeighborBuilder::getDestination() const {
  return destination_;
}

NeighborBuilder&
NeighborBuilder::setLinkAddress(const folly::MacAddress& linkAddress) {
  linkAddress_ = linkAddress;
  return *this;
}

std::optional<folly::MacAddress>
NeighborBuilder::getLinkAddress() const {
  return linkAddress_;
}

NeighborBuilder&
NeighborBuilder::setState(int state, bool deleted) {
  state_ = state;
  isReachable_ = deleted ? false : isNeighborReachable(state);
  return *this;
}

std::optional<int>
NeighborBuilder::getState() const {
  return state_;
}

bool
NeighborBuilder::getIsReachable() const {
  return isReachable_;
}

Neighbor::Neighbor(const NeighborBuilder& builder)
    : ifIndex_(builder.getIfIndex()),
      isReachable_(builder.getIsReachable()),
      destination_(builder.getDestination()),
      linkAddress_(builder.getLinkAddress()),
      state_(builder.getState()) {}

Neighbor::~Neighbor() {}

Neighbor::Neighbor(Neighbor&& other) noexcept {
  *this = std::move(other);
}

Neighbor&
Neighbor::operator=(Neighbor&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  ifIndex_ = other.ifIndex_;
  isReachable_ = other.isReachable_;
  destination_ = other.destination_;
  linkAddress_ = other.linkAddress_;
  state_ = other.state_;
  return *this;
}

Neighbor::Neighbor(const Neighbor& other) {
  *this = other;
}

Neighbor&
Neighbor::operator=(const Neighbor& other) {
  if (this == &other) {
    return *this;
  }

  ifIndex_ = other.ifIndex_;
  isReachable_ = other.isReachable_;
  destination_ = other.destination_;
  linkAddress_ = other.linkAddress_;
  state_ = other.state_;
  return *this;
}

int
Neighbor::getIfIndex() const {
  return ifIndex_;
}

int
Neighbor::getFamily() const {
  return destination_.family();
}

folly::IPAddress
Neighbor::getDestination() const {
  return destination_;
}

std::optional<folly::MacAddress>
Neighbor::getLinkAddress() const {
  return linkAddress_;
}

std::optional<int>
Neighbor::getState() const {
  return state_;
}

bool
Neighbor::isReachable() const {
  return isReachable_;
}

std::string
Neighbor::str() const {
  std::string stateStr{"n/a"};
  if (state_.has_value()) {
    stateStr = std::to_string(state_.value());
  }

  return fmt::format(
      "neighbor {} reachable {}, intf-index {}, mac-addr {}, state {}",
      destination_.str(),
      isReachable_ ? "Yes" : "No",
      ifIndex_,
      linkAddress_.has_value() ? linkAddress_->toString() : "n/a",
      stateStr);
}

bool
operator==(const Neighbor& lhs, const Neighbor& rhs) {
  return (
      lhs.getIfIndex() == rhs.getIfIndex() &&
      lhs.isReachable() == rhs.isReachable() &&
      lhs.getDestination() == rhs.getDestination() &&
      lhs.getLinkAddress() == rhs.getLinkAddress() &&
      lhs.getState() == rhs.getState());
}

/*================================GreInfo=====================================*/

GreInfo::GreInfo(
    const folly::IPAddress& localAddr,
    const folly::IPAddress& remoteAddr,
    uint8_t ttl)
    : localAddr_(localAddr), remoteAddr_(remoteAddr), ttl_(ttl) {}

GreInfo::~GreInfo() {}

GreInfo::GreInfo(GreInfo&& other) noexcept {
  *this = std::move(other);
}

GreInfo&
GreInfo::operator=(GreInfo&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  localAddr_ = std::move(other.localAddr_);
  remoteAddr_ = std::move(other.remoteAddr_);
  ttl_ = std::move(other.ttl_);
  return *this;
}

GreInfo::GreInfo(const GreInfo& other) {
  *this = other;
}

GreInfo&
GreInfo::operator=(const GreInfo& other) {
  if (this == &other) {
    return *this;
  }

  localAddr_ = other.localAddr_;
  remoteAddr_ = other.remoteAddr_;
  ttl_ = other.ttl_;
  return *this;
}

folly::IPAddress
GreInfo::getLocalAddr() const {
  return localAddr_;
}

folly::IPAddress
GreInfo::getRemoteAddr() const {
  return remoteAddr_;
}

uint8_t
GreInfo::getTtl() const {
  return ttl_;
}

std::string
GreInfo::str() const {
  return fmt::format(
      "GreInfo: localAddr {}, remoteAddr {}, ttl {}",
      localAddr_.str(),
      remoteAddr_.str(),
      ttl_);
}

bool
operator==(const GreInfo& lhs, const GreInfo& rhs) {
  return (
      lhs.getLocalAddr() == rhs.getLocalAddr() and
      lhs.getRemoteAddr() == rhs.getRemoteAddr() and
      lhs.getTtl() == rhs.getTtl());
}

/*==================================Link======================================*/

Link
LinkBuilder::build() const {
  return Link(*this);
}

LinkBuilder&
LinkBuilder::setLinkName(const std::string& linkName) {
  linkName_ = linkName;
  return *this;
}

const std::string&
LinkBuilder::getLinkName() const {
  return linkName_;
}

LinkBuilder&
LinkBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int
LinkBuilder::getIfIndex() const {
  return ifIndex_;
}

LinkBuilder&
LinkBuilder::setFlags(uint32_t flags) {
  flags_ = flags;
  return *this;
}

uint32_t
LinkBuilder::getFlags() const {
  return flags_;
}

LinkBuilder&
LinkBuilder::setLinkKind(const std::string& linkKind) {
  linkKind_ = linkKind;
  return *this;
}

std::optional<std::string>
LinkBuilder::getLinkKind() const {
  return linkKind_;
}

LinkBuilder&
LinkBuilder::setGreInfo(const GreInfo& greInfo) {
  greInfo_ = greInfo;
  return *this;
}

std::optional<GreInfo>
LinkBuilder::getGreInfo() const {
  return greInfo_;
}

Link::Link(const LinkBuilder& builder)
    : linkName_(builder.getLinkName()),
      ifIndex_(builder.getIfIndex()),
      flags_(builder.getFlags()),
      linkKind_(builder.getLinkKind()),
      greInfo_(builder.getGreInfo()) {}

Link::~Link() {}

Link::Link(Link&& other) noexcept {
  *this = std::move(other);
}

Link&
Link::operator=(Link&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  linkName_ = std::move(other.linkName_);
  ifIndex_ = std::move(other.ifIndex_);
  flags_ = std::move(other.flags_);
  linkKind_ = std::move(other.linkKind_);
  greInfo_ = std::move(other.greInfo_);
  return *this;
}

Link::Link(const Link& other) {
  *this = other;
}

Link&
Link::operator=(const Link& other) {
  if (this == &other) {
    return *this;
  }

  linkName_ = other.linkName_;
  ifIndex_ = other.ifIndex_;
  flags_ = other.flags_;
  linkKind_ = other.linkKind_;
  greInfo_ = other.greInfo_;
  return *this;
}

const std::string&
Link::getLinkName() const {
  return linkName_;
}

int
Link::getIfIndex() const {
  return ifIndex_;
}

uint32_t
Link::getFlags() const {
  return flags_;
}

bool
Link::isUp() const {
  return !!(flags_ & IFF_RUNNING);
}

bool
Link::isLoopback() const {
  return !!(flags_ & IFF_LOOPBACK);
}

std::optional<std::string>
Link::getLinkKind() const {
  return linkKind_;
}

std::optional<GreInfo>
Link::getGreInfo() const {
  return greInfo_;
}

std::string
Link::str() const {
  return fmt::format(
      "link {} intf-index {}, flags {}, kind {}, gre_info {}",
      linkName_,
      ifIndex_,
      std::to_string(flags_),
      linkKind_ ? linkKind_.value() : "n/a",
      greInfo_ ? greInfo_.value().str() : "n/a");
}

bool
operator==(const Link& lhs, const Link& rhs) {
  return (
      lhs.getLinkName() == rhs.getLinkName() and
      lhs.getIfIndex() == rhs.getIfIndex() and
      lhs.getFlags() == rhs.getFlags() and
      lhs.getLinkKind() == rhs.getLinkKind() and
      lhs.getGreInfo() == rhs.getGreInfo());
}

/*==================================Rule======================================*/

Rule::Rule(
    uint16_t family,
    uint8_t action,
    uint32_t table,
    std::optional<uint32_t> fwmark,
    std::optional<uint32_t> priority)
    : family_(family),
      action_(action),
      table_(table),
      fwmark_(fwmark),
      priority_(priority) {}

uint16_t
Rule::getFamily() const {
  return family_;
}

uint8_t
Rule::getAction() const {
  return action_;
}

void
Rule::setTable(uint32_t table) {
  table_ = table;
}

uint32_t
Rule::getTable() const {
  return table_;
}

void
Rule::setFwmark(uint32_t fwmark) {
  fwmark_ = fwmark;
}

std::optional<uint32_t>
Rule::getFwmark() const {
  return fwmark_;
}

void
Rule::setPriority(uint32_t priority) {
  priority_ = priority;
}

std::optional<uint32_t>
Rule::getPriority() const {
  return priority_;
}

std::string
Rule::str() const {
  return fmt::format(
      "rule family {}, action {}, table {}, fwmark {}, priority {}",
      family_,
      action_,
      table_,
      fwmark_ ? std::to_string(fwmark_.value()) : "n/a",
      priority_ ? std::to_string(priority_.value()) : "n/a");
}

bool
operator==(const Rule& lhs, const Rule& rhs) {
  return (
      lhs.getFamily() == rhs.getFamily() and
      lhs.getAction() == rhs.getAction() and
      lhs.getTable() == rhs.getTable() and
      lhs.getFwmark() == rhs.getFwmark() and
      lhs.getPriority() == rhs.getPriority());
}

} // namespace openr::fbnl
