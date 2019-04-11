/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <set>

#include <glog/logging.h>

#include <openr/nl/NetlinkTypes.h>

namespace openr {
namespace fbnl {

const std::set<int> kNeighborReachableStates{
    NUD_REACHABLE, NUD_STALE, NUD_DELAY, NUD_PERMANENT, NUD_PROBE, NUD_NOARP};
const int kIpAddrBufSize = 128;

bool
isNeighborReachable(int state) {
  return kNeighborReachableStates.count(state);
}

Route
RouteBuilder::build() const {
  return Route(*this);
}

Route
RouteBuilder::buildFromObject(struct rtnl_route* obj) {
  return loadFromObject(obj).build();
}

RouteBuilder&
RouteBuilder::loadFromObject(struct rtnl_route* obj) {
  CHECK_NOTNULL(obj);
  setScope(rtnl_route_get_scope(obj));
  setRouteTable(rtnl_route_get_table(obj));
  setFlags(rtnl_route_get_flags(obj));
  setProtocolId(rtnl_route_get_protocol(obj));
  setType(rtnl_route_get_type(obj));
  struct nl_addr* dst = rtnl_route_get_dst(obj);

  // Special handling for default routes
  // All others can be constructed from binary address form
  folly::CIDRNetwork prefix;
  std::array<char, kIpAddrBufSize> ipAddrBuf;
  if (nl_addr_get_prefixlen(dst) == 0) {
    if (nl_addr_get_family(dst) == AF_INET6) {
      VLOG(3) << "Creating a V6 default route";
      prefix = folly::IPAddress::createNetwork("::/0");
    } else if (nl_addr_get_family(dst) == AF_INET) {
      VLOG(3) << "Creating a V4 default route";
      prefix = folly::IPAddress::createNetwork("0.0.0.0/0");
    } else {
      throw fbnl::NlException("Unknown address family for default route");
    }
  } else {
    // route object dst is the prefix. parse it
    try {
      const auto ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
          static_cast<const unsigned char*>(nl_addr_get_binary_addr(dst)),
          nl_addr_get_len(dst)));
      prefix = {ipAddress, nl_addr_get_prefixlen(dst)};
    } catch (std::exception const& e) {
      throw fbnl::NlException(folly::sformat(
          "Error creating prefix for addr: {}",
          nl_addr2str(dst, ipAddrBuf.data(), ipAddrBuf.size())));
    }
  }
  setDestination(prefix);
  auto nextHopFunc = [](struct rtnl_nexthop * nhObj, void* ctx) noexcept->void {
    struct nl_addr* gw = rtnl_route_nh_get_gateway(nhObj);
    int ifIndex = rtnl_route_nh_get_ifindex(nhObj);
    // One of gateway or ifIndex must be set
    if (!gw && 0 == ifIndex) {
      return;
    }
    RouteBuilder* rtBuilder = reinterpret_cast<RouteBuilder*>(ctx);
    NextHopBuilder nhBuilder;
    rtBuilder->addNextHop(nhBuilder.buildFromObject(nhObj));
  };
  rtnl_route_foreach_nexthop(obj, nextHopFunc, static_cast<void*>(this));
  return *this;
}

Route
RouteBuilder::buildMulticastRoute() const {
  if (!routeIfIndex_.hasValue() || routeIfIndex_.value() == 0 ||
      !routeIfName_.hasValue()) {
    throw fbnl::NlException("Iface index and Iface name must be set");
  }
  NextHopBuilder nhBuilder;
  nhBuilder.setIfIndex(routeIfIndex_.value());

  RouteBuilder builder;
  return builder.setDestination(dst_)
      .setProtocolId(protocolId_)
      .setScope(scope_)
      .setType(RTN_MULTICAST)
      .setRouteIfName(routeIfName_.value())
      .addNextHop(nhBuilder.build())
      .build();
}

Route
RouteBuilder::buildLinkRoute() const {
  if (!routeIfIndex_.hasValue() || routeIfIndex_.value() == 0 ||
      !routeIfName_.hasValue()) {
    throw fbnl::NlException("Iface index and Iface name must be set");
  }
  NextHopBuilder nhBuilder;
  nhBuilder.setIfIndex(routeIfIndex_.value());

  RouteBuilder builder;
  return builder.setDestination(dst_)
      .setProtocolId(protocolId_)
      .setScope(RT_SCOPE_LINK)
      .setType(RTN_UNICAST)
      .setRouteIfName(routeIfName_.value())
      .addNextHop(nhBuilder.build())
      .build();
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

folly::Optional<uint32_t>
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
RouteBuilder::setRouteTable(uint8_t routeTable) {
  routeTable_ = routeTable;
  return *this;
}

uint8_t
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

folly::Optional<uint32_t>
RouteBuilder::getFlags() const {
  return flags_;
}

RouteBuilder&
RouteBuilder::setPriority(uint32_t priority) {
  priority_ = priority;
  return *this;
}

folly::Optional<uint32_t>
RouteBuilder::getPriority() const {
  return priority_;
}

RouteBuilder&
RouteBuilder::setTos(uint8_t tos) {
  tos_ = tos;
  return *this;
}

folly::Optional<uint8_t>
RouteBuilder::getTos() const {
  return tos_;
}

RouteBuilder&
RouteBuilder::setMtu(uint32_t mtu) {
  mtu_ = mtu;
  return *this;
}

folly::Optional<uint32_t>
RouteBuilder::getMtu() const {
  return mtu_;
}

RouteBuilder&
RouteBuilder::setAdvMss(uint32_t advMss) {
  advMss_ = advMss;
  return *this;
}

folly::Optional<uint32_t>
RouteBuilder::getAdvMss() const {
  return advMss_;
}

RouteBuilder&
RouteBuilder::setRouteIfName(const std::string& ifName) {
  routeIfName_ = ifName;
  return *this;
}

folly::Optional<std::string>
RouteBuilder::getRouteIfName() const {
  return routeIfName_;
}

RouteBuilder&
RouteBuilder::setRouteIfIndex(int ifIndex) {
  routeIfIndex_ = ifIndex;
  return *this;
}

folly::Optional<int>
RouteBuilder::getRouteIfIndex() const {
  return routeIfIndex_;
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

void
RouteBuilder::reset() {
  type_ = RTN_UNICAST;
  routeTable_ = RT_TABLE_MAIN;
  protocolId_ = DEFAULT_PROTOCOL_ID;
  scope_ = RT_SCOPE_UNIVERSE;
  isValid_ = false;
  flags_.clear();
  priority_.clear();
  tos_.clear();
  mtu_.clear();
  advMss_.clear();
  nextHops_.clear();
  routeIfName_.clear();
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
      routeIfName_(builder.getRouteIfName()),
      mplsLabel_(builder.getMplsLabel()) {}

Route::~Route() {
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
  if (routeKey_) {
    rtnl_route_put(routeKey_);
    routeKey_ = nullptr;
  }
}

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
  routeIfName_ = std::move(other.routeIfName_);
  family_ = std::move(other.family_);
  mplsLabel_ = std::move(other.mplsLabel_);
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
  if (other.route_) {
    route_ = other.route_;
    other.route_ = nullptr;
  }
  if (routeKey_) {
    rtnl_route_put(routeKey_);
    routeKey_ = nullptr;
  }
  if (other.routeKey_) {
    routeKey_ = other.routeKey_;
    other.routeKey_ = nullptr;
  }

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
  routeIfName_ = other.routeIfName_;
  family_ = other.family_;
  mplsLabel_ = other.mplsLabel_;
  // Free our route_ if any
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
  // Free our routeKey_ if any
  if (routeKey_) {
    rtnl_route_put(routeKey_);
    routeKey_ = nullptr;
  }

  // NOTE: We are not copying other.route_ so that this object can be passed
  // between threads without actually copying underlying netlink object to
  // other threads

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
       lhs.getRouteIfName() == rhs.getRouteIfName() &&
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

folly::Optional<uint32_t>
Route::getMplsLabel() const {
  return mplsLabel_;
}

folly::Optional<uint8_t>
Route::getTos() const {
  return tos_;
}

folly::Optional<uint32_t>
Route::getMtu() const {
  return mtu_;
}

folly::Optional<uint32_t>
Route::getAdvMss() const {
  return advMss_;
}

uint8_t
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

folly::Optional<uint32_t>
Route::getFlags() const {
  return flags_;
}

folly::Optional<uint32_t>
Route::getPriority() const {
  return priority_;
}

const NextHopSet&
Route::getNextHops() const {
  return nextHops_;
}

folly::Optional<std::string>
Route::getRouteIfName() const {
  return routeIfName_;
}

bool
Route::isValid() const {
  return isValid_;
}

std::string
Route::str() const {
  std::string result;
  if (family_ == AF_MPLS) {
    if (mplsLabel_.hasValue()) {
      result += folly::sformat("label {} ", mplsLabel_.value());
    }
  } else {
    result +=
        folly::sformat("route {} ", folly::IPAddress::networkToString(dst_));
  }
  uint32_t flags = 0;
  if (flags_.hasValue()) {
    flags = flags_.value();
  }
  result += folly::sformat(
      " proto {}, table {}, valid {}, family {}, flags {}, type {}",
      protocolId_,
      routeTable_,
      isValid_ ? "Yes" : "No",
      static_cast<int>(family_),
      flags,
      static_cast<int>(type_));

  if (priority_) {
    result += folly::sformat(", priority {}", priority_.value());
  }
  if (tos_) {
    result += folly::sformat(", tos {}", tos_.value());
  }
  if (mtu_) {
    result += folly::sformat(", mtu {}", mtu_.value());
  }
  if (advMss_) {
    result += folly::sformat(", advmss {}", advMss_.value());
  }
  for (auto const& nextHop : nextHops_) {
    result += "\n  " + nextHop.str();
  }
  return result;
}

struct rtnl_route*
Route::createRtnlRouteKey() {
  auto route = rtnl_route_alloc();
  if (route == nullptr) {
    throw fbnl::NlException("Cannot allocate route object");
  }
  SCOPE_FAIL {
    rtnl_route_put(route);
  };

  rtnl_route_set_scope(route, scope_);
  rtnl_route_set_type(route, type_);
  rtnl_route_set_family(route, dst_.first.family());
  rtnl_route_set_table(route, routeTable_);
  rtnl_route_set_protocol(route, protocolId_);

  // Set destination
  struct nl_addr* nlAddr = buildAddrObject(dst_);
  // route object takes a ref if dst is successfully set
  // so we should always drop our ref, success or failure
  SCOPE_EXIT {
    nl_addr_put(nlAddr);
  };
  int err = rtnl_route_set_dst(route, nlAddr);
  if (err != 0) {
    throw fbnl::NlException(folly::sformat(
        "Failed to set dst for route {} : {}",
        folly::IPAddress::networkToString(dst_),
        nl_geterror(err)));
  }

  return route;
}

struct rtnl_route*
Route::getRtnlRouteRef() {
  // Only build object once
  if (route_) {
    return route_;
  }

  route_ = createRtnlRouteKey();
  SCOPE_FAIL {
    rtnl_route_put(route_);
    route_ = nullptr;
  };

  if (priority_.hasValue()) {
    rtnl_route_set_priority(route_, priority_.value());
  }

  if (flags_.hasValue()) {
    rtnl_route_set_flags(route_, flags_.value());
  }

  if (tos_.hasValue()) {
    rtnl_route_set_tos(route_, tos_.value());
  }

  if (mtu_.hasValue()) {
    rtnl_route_set_metric(route_, RTAX_MTU, mtu_.value());
  }

  if (advMss_.hasValue()) {
    rtnl_route_set_metric(route_, RTAX_ADVMSS, advMss_.value());
  }

  // Add next hops
  // 1. check dst and nexthop's family
  for (const auto& nextHop : nextHops_) {
    auto gateway = nextHop.getGateway();
    if (gateway.hasValue() && gateway.value().family() != dst_.first.family()) {
      throw fbnl::NlException(
          "Different address family for destination and Nexthop gateway");
    }
  }
  // 2. build nexthop and add it to route
  for (auto nextHop : nextHops_) {
    rtnl_route_add_nexthop(route_, nextHop.getRtnlNexthopObj());
  }

  return route_;
}

struct rtnl_route*
Route::getRtnlRouteKeyRef() {
  // Only build object once
  if (routeKey_) {
    return routeKey_;
  }

  routeKey_ = createRtnlRouteKey();
  return routeKey_;
}

struct nl_addr*
Route::buildAddrObject(const folly::CIDRNetwork& addr) {
  struct nl_addr* nlAddr_ = nl_addr_build(
      addr.first.family(), (void*)(addr.first.bytes()), addr.first.byteCount());
  if (nlAddr_ == nullptr) {
    throw fbnl::NlException("Failed to create nl addr");
  }
  nl_addr_set_prefixlen(nlAddr_, addr.second);
  return nlAddr_;
}

/*=================================NextHop====================================*/

NextHop
NextHopBuilder::buildFromObject(struct rtnl_nexthop* obj) const {
  CHECK_NOTNULL(obj);
  NextHopBuilder builder;

  // set ifindex, rtnl_nexthop defaut ifindex to 0
  auto ifindex = rtnl_route_nh_get_ifindex(obj);
  if (ifindex != 0) {
    builder.setIfIndex(ifindex);
  }

  // Get the gateway IP from nextHop
  struct nl_addr* gw = rtnl_route_nh_get_gateway(obj);
  if (!gw) {
    return builder.build();
  }
  auto gwAddr = folly::IPAddress::fromBinary(folly::ByteRange(
      (const unsigned char*)nl_addr_get_binary_addr(gw), nl_addr_get_len(gw)));
  builder.setGateway(gwAddr).setWeight(rtnl_route_nh_get_weight(obj));
  return builder.build();
}

NextHop
NextHopBuilder::build() const {
  return NextHop(*this);
}

void
NextHopBuilder::reset() {
  ifIndex_.clear();
  weight_.clear();
  gateway_.clear();
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

folly::Optional<int>
NextHopBuilder::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress>
NextHopBuilder::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t>
NextHopBuilder::getWeight() const {
  return weight_;
}

folly::Optional<thrift::MplsActionCode>
NextHopBuilder::getLabelAction() const {
  return labelAction_;
}

folly::Optional<uint32_t>
NextHopBuilder::getSwapLabel() const {
  return swapLabel_;
}

folly::Optional<std::vector<int32_t>>
NextHopBuilder::getPushLabels() const {
  return pushLabels_;
}

uint8_t
NextHopBuilder::getFamily() const {
  if (gateway_.hasValue()) {
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
      lhs.getWeight() == rhs.getWeight() &&
      lhs.getLabelAction() == rhs.getLabelAction() &&
      lhs.getSwapLabel() == rhs.getSwapLabel() &&
      lhs.getPushLabels() == rhs.getPushLabels() &&
      lhs.getFamily() == rhs.getFamily();
}

size_t
NextHopHash::operator()(const NextHop& nh) const {
  size_t res = 0;
  if (nh.getIfIndex().hasValue()) {
    res += std::hash<std::string>()(std::to_string(nh.getIfIndex().value()));
  }
  if (nh.getGateway().hasValue()) {
    res += std::hash<std::string>()(nh.getGateway().value().str());
  }
  if (nh.getWeight().hasValue()) {
    res += std::hash<std::string>()(std::to_string(nh.getWeight().value()));
  }
  return res;
}

folly::Optional<int>
NextHop::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress>
NextHop::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t>
NextHop::getWeight() const {
  return weight_;
}

folly::Optional<thrift::MplsActionCode>
NextHop::getLabelAction() const {
  return labelAction_;
}

folly::Optional<uint32_t>
NextHop::getSwapLabel() const {
  return swapLabel_;
}

folly::Optional<std::vector<int32_t>>
NextHop::getPushLabels() const {
  return pushLabels_;
}

uint8_t
NextHop::getFamily() const {
  if (gateway_.hasValue()) {
    return gateway_.value().family();
  }
  return AF_UNSPEC;
}

std::string
NextHop::str() const {
  std::string result;
  result += folly::sformat(
      "nexthop via {}, intf-index {}, weight {}",
      (gateway_ ? gateway_->str() : "n/a"),
      (ifIndex_ ? std::to_string(*ifIndex_) : "n/a"),
      (weight_ ? std::to_string(*weight_) : "n/a"));
  if (labelAction_.hasValue()) {
    result += folly::sformat(
        " Label action {}",
        apache::thrift::util::enumNameSafe(labelAction_.value()));
  }
  if (swapLabel_.hasValue()) {
    result += folly::sformat(" Swap label {}", swapLabel_.value());
  }
  if (pushLabels_.hasValue()) {
    result += " Push Labels: ";
    for (const auto& label : pushLabels_.value()) {
      result += folly::sformat(" {} ", label);
    }
  }
  return result;
}

struct rtnl_nexthop*
NextHop::getRtnlNexthopObj() const {
  if (ifIndex_.hasValue() && gateway_.hasValue()) {
    return buildNextHopInternal(ifIndex_.value(), gateway_.value());
  } else if (ifIndex_.hasValue()) {
    return buildNextHopInternal(ifIndex_.value());
  } else if (gateway_.hasValue()) {
    return buildNextHopInternal(gateway_.value());
  }
  throw NlException("Invalid nexthop");
}

struct rtnl_nexthop*
NextHop::buildNextHopInternal(const int ifIdx) const {
  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw fbnl::NlException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_ifindex(nextHop, ifIdx);
  return nextHop;
}

struct rtnl_nexthop*
NextHop::buildNextHopInternal(
    int ifIdx, const folly::IPAddress& gateway) const {
  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());
  if (nlGateway == nullptr) {
    throw fbnl::NlException("Failed to create nl addr for gateway");
  }
  // nextHop object takes a ref if gateway is successfully set
  // Either way, success or failure, we drop our ref
  SCOPE_EXIT {
    nl_addr_put(nlGateway);
  };

  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw fbnl::NlException("Failed to create nextHop");
  }

  if (gateway.isV4()) {
    rtnl_route_nh_set_flags(nextHop, RTNH_F_ONLINK);
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }

  rtnl_route_nh_set_ifindex(nextHop, ifIdx);
  rtnl_route_nh_set_gateway(nextHop, nlGateway);
  return nextHop;
}

// build nexthop with nexthop = global ip addresses
struct rtnl_nexthop*
NextHop::buildNextHopInternal(const folly::IPAddress& gateway) const {
  if (gateway.isLinkLocal()) {
    throw fbnl::NlException(folly::sformat(
        "Missing interface name for link local nexthop address {}",
        gateway.str()));
  }

  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());
  if (nlGateway == nullptr) {
    throw fbnl::NlException("Failed to create nl addr for gateway");
  }
  // nextHop object takes a ref if gateway is successfully set
  // Either way, success or failure, we drop our ref
  SCOPE_EXIT {
    nl_addr_put(nlGateway);
  };

  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw fbnl::NlException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_gateway(nextHop, nlGateway);
  return nextHop;
}

/*================================IfAddress===================================*/

IfAddress
IfAddressBuilder::build() const {
  return IfAddress(*this);
}

IfAddress
IfAddressBuilder::buildFromObject(struct rtnl_addr* addr) {
  return loadFromObject(addr).build();
}

IfAddressBuilder&
IfAddressBuilder::loadFromObject(struct rtnl_addr* addr) {
  reset();
  struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
  if (!ipaddr) {
    throw fbnl::NlException("Failed to get ip address");
  }
  folly::IPAddress ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(ipaddr)),
      nl_addr_get_len(ipaddr)));

  uint8_t prefixLen = nl_addr_get_prefixlen(ipaddr);
  setIfIndex(rtnl_addr_get_ifindex(addr));
  setFlags(rtnl_addr_get_flags(addr));
  setPrefix(std::make_pair(ipAddress, prefixLen));
  return *this;
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

folly::Optional<folly::CIDRNetwork>
IfAddressBuilder::getPrefix() const {
  return prefix_;
}

IfAddressBuilder&
IfAddressBuilder::setFamily(uint8_t family) {
  family_ = family;
  return *this;
}

// Family will be shadowed if prefix is set
folly::Optional<uint8_t>
IfAddressBuilder::getFamily() const {
  return family_;
}

IfAddressBuilder&
IfAddressBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

folly::Optional<uint8_t>
IfAddressBuilder::getScope() const {
  return scope_;
}

IfAddressBuilder&
IfAddressBuilder::setFlags(uint8_t flags) {
  flags_ = flags;
  return *this;
}

folly::Optional<uint8_t>
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
  prefix_.clear();
  scope_.clear();
  flags_.clear();
  family_.clear();
}

IfAddress::IfAddress(const IfAddressBuilder& builder)
    : prefix_(builder.getPrefix()),
      ifIndex_(builder.getIfIndex()),
      isValid_(builder.isValid()),
      scope_(builder.getScope()),
      flags_(builder.getFlags()),
      family_(builder.getFamily()) {}

IfAddress::~IfAddress() {
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
}

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
  // release old object
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
  if (other.ifAddr_) {
    ifAddr_ = other.ifAddr_;
    other.ifAddr_ = nullptr;
  }

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
  // release old object
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
  // NOTE: We are not copying other.ifAddr_ so that this object can be
  // passed between threads without actually copying underlying netlink object
  // to other threads

  return *this;
}

uint8_t
IfAddress::getFamily() const {
  if (prefix_.hasValue()) {
    return prefix_->first.family();
  } else {
    return family_.value();
  }
}

uint8_t
IfAddress::getPrefixLen() const {
  if (prefix_.hasValue()) {
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

folly::Optional<folly::CIDRNetwork>
IfAddress::getPrefix() const {
  return prefix_;
}

folly::Optional<uint8_t>
IfAddress::getScope() const {
  return scope_;
}

folly::Optional<uint8_t>
IfAddress::getFlags() const {
  return flags_;
}

std::string
IfAddress::str() const {
  return folly::sformat(
      "addr {} {} intf-index {}, valid {}",
      getFamily() == AF_INET ? "inet" : "inet6",
      prefix_.hasValue() ? folly::IPAddress::networkToString(*prefix_) : "n/a",
      ifIndex_,
      isValid_ ? "Yes" : "No");
}

// Will construct rtnl_addr object on the first time call, then will return
// the same object pointer
struct rtnl_addr*
IfAddress::getRtnlAddrRef() {
  if (ifAddr_) {
    return ifAddr_;
  }

  ifAddr_ = rtnl_addr_alloc();
  if (nullptr == ifAddr_) {
    throw fbnl::NlException("Failed to create rtnl_addr object");
  }
  SCOPE_FAIL {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  };
  rtnl_addr_set_ifindex(ifAddr_, ifIndex_);

  // Get local addr
  if (prefix_.hasValue()) {
    struct nl_addr* localAddr = nl_addr_build(
        prefix_->first.family(),
        (void*)(prefix_->first.bytes()),
        prefix_->first.byteCount());
    if (nullptr == localAddr) {
      throw fbnl::NlException("Failed to create local addr");
    }
    nl_addr_set_prefixlen(localAddr, prefix_->second);
    // Setting the local address will automatically set the address family
    // and the prefix length to the correct values.
    // rtnl_addr_set_local will increase reference for localAddr so we will
    // drop our reference afterwards.
    rtnl_addr_set_local(ifAddr_, localAddr);
    nl_addr_put(localAddr);
  }

  if (family_.hasValue()) {
    rtnl_addr_set_family(ifAddr_, family_.value());
  }
  if (scope_.hasValue()) {
    rtnl_addr_set_scope(ifAddr_, scope_.value());
  }
  if (flags_.hasValue()) {
    rtnl_addr_set_flags(ifAddr_, flags_.value());
  }

  return ifAddr_;
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

Neighbor
NeighborBuilder::buildFromObject(
    struct rtnl_neigh* neighbor, bool deleted) const {
  CHECK_NOTNULL(neighbor);
  NeighborBuilder builder;
  // The destination IP
  struct nl_addr* dst = rtnl_neigh_get_dst(neighbor);
  if (!dst) {
    LOG(ERROR) << "Invalid destination for neighbor";
    throw fbnl::NlException("Failed to get destination IP from neighbor entry");
  }
  const auto ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
      static_cast<const unsigned char*>(nl_addr_get_binary_addr(dst)),
      nl_addr_get_len(dst)));
  int state = rtnl_neigh_get_state(neighbor);
  bool isReachable = deleted ? false : isNeighborReachable(state);
  builder.setDestination(ipAddress)
      .setIfIndex(rtnl_neigh_get_ifindex(neighbor))
      .setState(state, deleted);

  // link address exists only for reachable states, so it may not
  // always exist
  folly::MacAddress macAddress;
  if (isReachable) {
    struct nl_addr* linkAddress = rtnl_neigh_get_lladdr(neighbor);
    if (!linkAddress) {
      LOG(ERROR) << "Invalid link address for neigbbor";
      throw fbnl::NlException("Failed to get link address from neighbor entry");
    }
    // Skip entries with invalid mac-addresses
    if (nl_addr_get_len(linkAddress) != folly::MacAddress::SIZE) {
      LOG(ERROR) << "Invalid link address for neigbbor";
      throw fbnl::NlException("Invalid mac address");
    }
    macAddress = folly::MacAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(linkAddress)),
        nl_addr_get_len(linkAddress)));
  }
  builder.setLinkAddress(macAddress);

  std::array<char, 128> stateBuf = {""};
  VLOG(4) << "Built neighbor entry: "
          << " family " << rtnl_neigh_get_family(neighbor) << " IfIndex "
          << rtnl_neigh_get_ifindex(neighbor) << " : " << ipAddress.str()
          << " -> " << macAddress.toString() << " state "
          << rtnl_neigh_state2str(state, stateBuf.data(), stateBuf.size());

  return builder.build();
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

folly::Optional<folly::MacAddress>
NeighborBuilder::getLinkAddress() const {
  return linkAddress_;
}

NeighborBuilder&
NeighborBuilder::setState(int state, bool deleted) {
  state_ = state;
  isReachable_ = deleted ? false : isNeighborReachable(state);
  return *this;
}

folly::Optional<int>
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

Neighbor::~Neighbor() {
  if (neigh_) {
    rtnl_neigh_put(neigh_);
    neigh_ = nullptr;
  }
}

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
  if (neigh_) {
    rtnl_neigh_put(neigh_);
    neigh_ = nullptr;
  }
  if (other.neigh_) {
    neigh_ = other.neigh_;
    other.neigh_ = nullptr;
  }

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
  if (neigh_) {
    rtnl_neigh_put(neigh_);
    neigh_ = nullptr;
  }
  // NOTE: We are not copying other.neigh_ so that this object can be
  // passed between threads without actually copying underlying netlink object
  // to other threads

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

folly::Optional<folly::MacAddress>
Neighbor::getLinkAddress() const {
  return linkAddress_;
}

folly::Optional<int>
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
  if (state_.hasValue()) {
    std::array<char, 32> buf;
    rtnl_neigh_state2str(state_.value(), buf.data(), buf.size());
    stateStr = std::string(buf.data(), buf.size());
  }

  return folly::sformat(
      "neighbor {} reachable {}, intf-index {}, lladdr {}, state {}",
      destination_.str(),
      isReachable_ ? "Yes" : "No",
      ifIndex_,
      linkAddress_.hasValue() ? linkAddress_->toString() : "n/a",
      stateStr);
}

struct rtnl_neigh*
Neighbor::getRtnlNeighRef() {
  if (neigh_) {
    return neigh_;
  }

  neigh_ = rtnl_neigh_alloc();
  if (!neigh_) {
    throw fbnl::NlException("create neighbor object failed");
  }
  SCOPE_FAIL {
    rtnl_neigh_put(neigh_);
  };

  rtnl_neigh_set_ifindex(neigh_, ifIndex_);
  if (state_.hasValue()) {
    rtnl_neigh_set_state(neigh_, state_.value());
  }

  struct nl_addr* dst = nl_addr_build(
      destination_.family(),
      (void*)(destination_.bytes()),
      destination_.byteCount());
  if (dst == nullptr) {
    throw fbnl::NlException("Failed to create dst addr");
  }
  SCOPE_EXIT {
    nl_addr_put(dst);
  };
  rtnl_neigh_set_dst(neigh_, dst);

  if (linkAddress_.hasValue()) {
    struct nl_addr* llAddr = nl_addr_build(
        AF_UNSPEC,
        (void*)linkAddress_.value().bytes(),
        folly::MacAddress::SIZE);
    if (llAddr == nullptr) {
      throw fbnl::NlException("Failed to create link addr");
    }
    rtnl_neigh_set_lladdr(neigh_, llAddr);
    nl_addr_put(llAddr);
  }

  return neigh_;
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

/*==================================Link======================================*/

Link
LinkBuilder::buildFromObject(struct rtnl_link* link) const {
  CHECK_NOTNULL(link);
  std::string linkName("unknown");
  const char* linkNameStr = rtnl_link_get_name(link);
  if (linkNameStr) {
    linkName.assign(linkNameStr);
  }

  LinkBuilder builder;
  builder.setIfIndex(rtnl_link_get_ifindex(link))
      .setFlags(rtnl_link_get_flags(link))
      .setLinkName(linkName);
  return builder.build();
}

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

Link::Link(const LinkBuilder& builder)
    : linkName_(builder.getLinkName()),
      ifIndex_(builder.getIfIndex()),
      flags_(builder.getFlags()) {}

Link::~Link() {
  if (link_) {
    rtnl_link_put(link_);
    link_ = nullptr;
  }
}

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
  if (link_) {
    rtnl_link_put(link_);
    link_ = nullptr;
  }
  if (other.link_) {
    link_ = other.link_;
    other.link_ = nullptr;
  }

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

  if (link_) {
    rtnl_link_put(link_);
    link_ = nullptr;
  }
  // NOTE: We are not copying other.link_ so that this object can be
  // passed between threads without actually copying underlying netlink object
  // to other threads

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

struct rtnl_link*
Link::getRtnlLinkRef() {
  if (link_) {
    return link_;
  }

  link_ = rtnl_link_alloc();
  if (!link_) {
    throw fbnl::NlException("Allocate link object failed");
  }
  rtnl_link_set_ifindex(link_, ifIndex_);
  rtnl_link_set_flags(link_, flags_);
  rtnl_link_set_name(link_, linkName_.c_str());

  return link_;
}

bool
Link::isUp() const {
  return !!(flags_ & IFF_RUNNING);
}

bool
Link::isLoopback() const {
  return !!(flags_ & IFF_LOOPBACK);
}

std::string
Link::str() const {
  std::array<char, 128> flagsBuf;
  rtnl_link_flags2str(flags_, flagsBuf.data(), flagsBuf.size());

  return folly::sformat(
      "link {} intf-index {}, flags {}",
      linkName_,
      ifIndex_,
      std::string(flagsBuf.data(), flagsBuf.size()));
}

bool
operator==(const Link& lhs, const Link& rhs) {
  return (
      lhs.getLinkName() == rhs.getLinkName() &&
      lhs.getIfIndex() == rhs.getIfIndex() && lhs.getFlags() == rhs.getFlags());
}

} // namespace fbnl
} // namespace openr
