#include "NetlinkTypes.h"
#include "NetlinkException.h"

#include <set>

namespace openr {
namespace fbnl {

const std::set<int> kNeighborReachableStates{
    NUD_REACHABLE, NUD_STALE, NUD_DELAY, NUD_PERMANENT, NUD_PROBE, NUD_NOARP};
const int kIpAddrBufSize = 128;

bool isNeighborReachable(int state) {
  return kNeighborReachableStates.count(state);
}

Route RouteBuilder::buildRoute() const {
  return Route(*this);
}

Route RouteBuilder::buildFromObject(struct rtnl_route* obj) {
  return loadFromObject(obj).buildRoute();
}

RouteBuilder& RouteBuilder::loadFromObject(struct rtnl_route* obj) {
  CHECK_NOTNULL(obj);
  uint32_t scope = rtnl_route_get_scope(obj);
  uint32_t table = rtnl_route_get_table(obj);
  uint32_t flags = rtnl_route_get_flags(obj);
  uint32_t proto = rtnl_route_get_protocol(obj);
  uint32_t type = rtnl_route_get_type(obj);
  setScope(scope);
  setRouteTable(table);
  setFlags(flags);
  setProtocolId(proto);
  setType(type);
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
      throw NetlinkException("Unknown address family for default route");
    }
  } else {
    // route object dst is the prefix. parse it
    try {
      const auto ipAddress = folly::IPAddress::fromBinary(folly::ByteRange(
          static_cast<const unsigned char*>(nl_addr_get_binary_addr(dst)),
          nl_addr_get_len(dst)));
      prefix = {ipAddress, nl_addr_get_prefixlen(dst)};
    } catch (std::exception const& e) {
      throw NetlinkException(folly::sformat(
        "Error creating prefix for addr: {}",
        nl_addr2str(dst, ipAddrBuf.data(), ipAddrBuf.size())));
    }
  }
  setDestination(prefix);
  auto nextHopFunc = [](struct rtnl_nexthop * obj, void* ctx) noexcept->void {
    struct nl_addr* gw = rtnl_route_nh_get_gateway(obj);
    int ifIndex = rtnl_route_nh_get_ifindex(obj);
    // One of gateway or ifIndex must be set
    if (!gw && 0 == ifIndex) {
      return;
    }
    RouteBuilder* rtBuilder = reinterpret_cast<RouteBuilder*>(ctx);
    NextHopBuilder nhBuilder;
    rtBuilder->addNextHop(std::move(nhBuilder.buildFromObject(obj)));
  };
  rtnl_route_foreach_nexthop(obj, nextHopFunc, static_cast<void*>(this));
  return *this;
}

Route RouteBuilder::buildMulticastRoute() const {
  if (!routeIfIndex_.hasValue() || routeIfIndex_.value() == 0
   || !routeIfName_.hasValue()) {
    throw NetlinkException("Iface index and Iface name must be set");
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
                .buildRoute();
}

Route RouteBuilder::buildLinkRoute() const {
  if (!routeIfIndex_.hasValue() || routeIfIndex_.value() == 0
   || !routeIfName_.hasValue()) {
    throw NetlinkException("Iface index and Iface name must be set");
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
                .buildRoute();
}

RouteBuilder& RouteBuilder::setDestination(const folly::CIDRNetwork& dst) {
  dst_ = dst;
  return *this;
}

const folly::CIDRNetwork& RouteBuilder::getDestination() const {
  return dst_;
}

RouteBuilder& RouteBuilder::setType(uint8_t type) {
  type_ = type;
  return *this;
}

uint8_t RouteBuilder::getType() const {
  return type_;
}

RouteBuilder& RouteBuilder::setRouteTable(uint8_t routeTable) {
  routeTable_ = routeTable;
  return *this;
}

uint8_t RouteBuilder::getRouteTable() const {
  return routeTable_;
}

RouteBuilder& RouteBuilder::setProtocolId(uint8_t protocolId) {
  protocolId_ = protocolId;
  return *this;
}

uint8_t RouteBuilder::getProtocolId() const {
  return protocolId_;
}

RouteBuilder& RouteBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

uint8_t RouteBuilder::getScope() const {
  return scope_;
}

 // Optional parameters set after object is constructed
RouteBuilder& RouteBuilder::setFlags(uint32_t flags) {
  flags_ = flags;
  return *this;
}

folly::Optional<uint32_t> RouteBuilder::getFlags() const {
  return flags_;
}

RouteBuilder& RouteBuilder::setPriority(uint32_t priority) {
  priority_ = priority;
  return *this;
}

folly::Optional<uint32_t> RouteBuilder::getPriority() const {
  return priority_;
}

RouteBuilder& RouteBuilder::setTos(uint8_t tos) {
  tos_ = tos;
  return *this;
}

folly::Optional<uint8_t> RouteBuilder::getTos() const {
  return tos_;
}

RouteBuilder& RouteBuilder::setRouteIfName(const std::string& ifName) {
  routeIfName_ = ifName;
  return *this;
}

folly::Optional<std::string> RouteBuilder::getRouteIfName() const {
  return routeIfName_;
}

RouteBuilder& RouteBuilder::setRouteIfIndex(int ifIndex) {
  routeIfIndex_ = ifIndex;
  return *this;
}

folly::Optional<int> RouteBuilder::getRouteIfIndex() const {
  return routeIfIndex_;
}

RouteBuilder& RouteBuilder::addNextHop(const NextHop& nextHop) {
  nextHops_.emplace(nextHop);
  return *this;
}

const NextHopSet&
RouteBuilder::getNextHops() const {
  return nextHops_;
}

RouteBuilder& RouteBuilder::setValid(bool isValid) {
  isValid_ = isValid;
  return *this;
}

bool RouteBuilder::isValid() const {
  return isValid_;
}

void RouteBuilder::reset() {
  type_ = RTN_UNICAST;
  routeTable_ = RT_TABLE_MAIN;
  protocolId_ = DEFAULT_PROTOCOL_ID;
  scope_ = RT_SCOPE_UNIVERSE;
  isValid_ = false;
  flags_.clear();
  priority_.clear();
  tos_.clear();
  nextHops_.clear();
  routeIfName_.clear();
}

Route::Route(const RouteBuilder& builder)
  : type_(builder.getType()),
    routeTable_(builder.getRouteTable()),
    protocolId_(builder.getProtocolId()),
    scope_(builder.getScope()),
    isValid_(builder.isValid()),
    flags_(builder.getFlags()),
    priority_(builder.getPriority()),
    tos_(builder.getTos()),
    nextHops_(builder.getNextHops()),
    dst_ (builder.getDestination()),
    routeIfName_(builder.getRouteIfName()) {
  init();
}

Route::~Route() {
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
}

Route::Route(Route&& other) noexcept
  : type_(other.type_),
    routeTable_(other.routeTable_),
    protocolId_(other.protocolId_),
    scope_(other.scope_),
    isValid_(other.isValid_),
    flags_(other.flags_),
    priority_(other.priority_),
    tos_(other.tos_),
    nextHops_(other.nextHops_),
    dst_ (other.dst_),
    routeIfName_(other.routeIfName_) {
  if (other.route_) {
    // prevent double release
    route_ = other.route_;
    other.route_ = nullptr;
  }
}

Route& Route::operator=(Route&& other) noexcept {
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
  nextHops_ = other.nextHops_;
  dst_ = other.dst_;
  routeIfName_ = other.routeIfName_;
  if (route_) {
    rtnl_route_put(route_);
    route_ = nullptr;
  }
  if (other.route_) {
    route_ = other.route_;
    other.route_ = nullptr;
  }
  return *this;
}

bool operator==(const Route& lhs, const Route& rhs) {
  bool ret = lhs.getType() == rhs.getType()
      && lhs.getTos() == rhs.getTos()
      && lhs.getFlags() == rhs.getFlags()
      && lhs.getScope() == rhs.getScope()
      && lhs.isValid() == rhs.isValid()
      && lhs.getDestination() == rhs.getDestination()
      && lhs.getPriority() == rhs.getPriority()
      && lhs.getNextHops().size() == rhs.getNextHops().size();

  if (!ret) {
    return false;
  }
  for (const NextHop& nh : lhs.getNextHops()) {
    if (!rhs.getNextHops().count(nh)) {
      return false;
    }
  }
  return true;
}

uint8_t Route::getFamily() const {
  return dst_.first.family();
}

uint8_t Route::getType() const {
  return type_;
}

const folly::CIDRNetwork& Route::getDestination() const {
  return dst_;
}

folly::Optional<uint8_t> Route::getTos() const {
  return tos_;
}

uint8_t Route::getRouteTable() const {
  return routeTable_;
}

uint8_t Route::getProtocolId() const {
  return protocolId_;
}

uint8_t Route::getScope() const {
  return scope_;
}

folly::Optional<uint32_t> Route::getFlags() const {
  return flags_;
}

folly::Optional<uint32_t> Route::getPriority() const {
  return priority_;
}

const NextHopSet&
Route::getNextHops() const {
  return nextHops_;
}

folly::Optional<std::string> Route::getRouteIfName() const {
  return routeIfName_;
}

bool Route::isValid() const {
  return isValid_;
}

struct rtnl_route* Route::fromNetlinkRoute() const {
  return route_;
}

void Route::init() {
  VLOG(4) << "Creating route object";

  // Only build object once
  if (route_) {
    return;
  }
  route_ = rtnl_route_alloc();
  if (route_ == nullptr) {
    throw NetlinkException("Cannot allocate route object");
  }

  SCOPE_FAIL {
    rtnl_route_put(route_);
    route_ = nullptr;
  };

  rtnl_route_set_scope(route_, scope_);
  rtnl_route_set_type(route_, type_);
  rtnl_route_set_family(route_, dst_.first.family());
  rtnl_route_set_table(route_, routeTable_);
  rtnl_route_set_protocol(route_, protocolId_);

  if (priority_.hasValue()) {
    rtnl_route_set_priority(route_, priority_.value());
  }

  if (flags_.hasValue()) {
    rtnl_route_set_flags(route_, flags_.value());
  }

  if (tos_.hasValue()) {
    rtnl_route_set_tos(route_, tos_.value());
  }

  // Set destination
  struct nl_addr* nlAddr = buildAddrObject(dst_);
  // route object takes a ref if dst is successfully set
  // so we should always drop our ref, success or failure
  SCOPE_EXIT {
    nl_addr_put(nlAddr);
  };
  int err = rtnl_route_set_dst(route_, nlAddr);
  if (err != 0) {
    throw NetlinkException(folly::sformat(
        "Failed to set dst for route {} : {}",
        folly::IPAddress::networkToString(dst_),
        nl_geterror(err)));
  }

  if (nextHops_.empty()) {
    return;
  }
  // Add next hops
  // 1. check dst and nexthop's family
  for (const auto& nextHop : nextHops_) {
    auto gateway = nextHop.getGateway();
    if (gateway.hasValue()
     && gateway.value().family() != dst_.first.family()) {
      throw NetlinkException(
        "Different address family for destination and Nexthop gateway");
    }
  }
  // 2. build nexthop and add it to route
  for (auto nextHop : nextHops_) {
    struct rtnl_nexthop* nh = nextHop.fromNetlinkNextHop();
    rtnl_route_add_nexthop(route_, nh);
  }
}

struct nl_addr* Route::buildAddrObject(const folly::CIDRNetwork& addr) {
  struct nl_addr* nlAddr_ = nl_addr_build(
      addr.first.family(),
      (void*)(addr.first.bytes()),
      addr.first.byteCount());
  if (nlAddr_ == nullptr) {
    throw NetlinkException("Failed to create nl addr");
  }
  nl_addr_set_prefixlen(nlAddr_, addr.second);
  return nlAddr_;
}

/*=================================NextHop====================================*/

NextHop NextHopBuilder::buildFromObject(struct rtnl_nexthop* obj) const {
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
  builder.setGateway(gwAddr)
         .setWeight(rtnl_route_nh_get_weight(obj));
  return builder.build();
}

NextHop NextHopBuilder::build() const {
  return NextHop(*this);
}

void NextHopBuilder::reset() {
  ifIndex_.clear();
  weight_.clear();
  gateway_.clear();
}

NextHopBuilder& NextHopBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

NextHopBuilder&
NextHopBuilder::setGateway(const folly::IPAddress& gateway) {
  gateway_ = gateway;
  return *this;
}

NextHopBuilder& NextHopBuilder::setWeight(uint8_t weight) {
  weight_ = weight;
  return *this;
}

folly::Optional<int> NextHopBuilder::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress> NextHopBuilder::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t> NextHopBuilder::getWeight() const {
  return weight_;
}

NextHop::NextHop(const NextHopBuilder& builder)
  : ifIndex_(builder.getIfIndex()),
    gateway_(builder.getGateway()),
    weight_(builder.getWeight()) {
  init();
}

bool operator==(const NextHop& lhs, const NextHop& rhs) {
  return lhs.getIfIndex() == rhs.getIfIndex()
      && lhs.getGateway() == rhs.getGateway()
      && lhs.getWeight() == rhs.getWeight();
}

size_t NextHopHash::operator()(const NextHop& nh) const {
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

folly::Optional<int> NextHop::getIfIndex() const {
  return ifIndex_;
}

folly::Optional<folly::IPAddress> NextHop::getGateway() const {
  return gateway_;
}

folly::Optional<uint8_t> NextHop::getWeight() const {
  return weight_;
}

void NextHop::init() {
  if (nextHop_) {
    return;
  }
  if (ifIndex_.hasValue() && gateway_.hasValue()) {
    nextHop_ = buildNextHopInternal(ifIndex_.value(), gateway_.value());
  } else if (ifIndex_.hasValue()) {
    nextHop_ = buildNextHopInternal(ifIndex_.value());
  } else if (gateway_.hasValue()) {
    nextHop_ = buildNextHopInternal(gateway_.value());
  }
}

struct rtnl_nexthop* NextHop::fromNetlinkNextHop() const {
  return nextHop_;
}

void NextHop::release() {
  if (nextHop_) {
    rtnl_route_nh_free(nextHop_);
    nextHop_ = nullptr;
  }
}

std::string
NextHop::str() const {
  return folly::sformat(
      "Nexthop Info: Gateway = {}, Interface index = {}, Weight = {}",
      (getGateway() ? getGateway()->str() : "Null"),
      (getIfIndex() ? std::to_string(*getIfIndex()) : "Null"),
      (getWeight() ? std::to_string(*getWeight()) : "Null"));
}

struct rtnl_nexthop* NextHop::buildNextHopInternal(const int ifIdx) {
  // We create a nextHop oject here but by adding it to route
  // the route object owns it
  // Once we destroy the route object, it will internally free this nextHop
  struct rtnl_nexthop* nextHop = rtnl_route_nh_alloc();
  if (nextHop == nullptr) {
    throw NetlinkException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_ifindex(nextHop, ifIdx);
  return nextHop;
}

struct rtnl_nexthop* NextHop::buildNextHopInternal(
  int ifIdx, const folly::IPAddress& gateway) {
  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

  if (nlGateway == nullptr) {
    throw NetlinkException("Failed to create nl addr for gateway");
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
    throw NetlinkException("Failed to create nextHop");
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
struct rtnl_nexthop* NextHop::buildNextHopInternal(
  const folly::IPAddress& gateway) {
  if (gateway.isLinkLocal()) {
    throw NetlinkException(folly::sformat(
        "Failed to resolve interface name for link local address {}",
        gateway.str()));
  }

  struct nl_addr* nlGateway = nl_addr_build(
      gateway.family(), (void*)(gateway.bytes()), gateway.byteCount());

  if (nlGateway == nullptr) {
    throw NetlinkException("Failed to create nl addr for gateway");
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
    throw NetlinkException("Failed to create nextHop");
  }
  if (weight_.hasValue()) {
    rtnl_route_nh_set_weight(nextHop, weight_.value());
  }
  rtnl_route_nh_set_gateway(nextHop, nlGateway);
  return nextHop;
}

/*================================IfAddress===================================*/

IfAddress IfAddressBuilder::build() const {
  return IfAddress(*this);
}

IfAddress IfAddressBuilder::buildFromObject(struct rtnl_addr* addr) {
  return loadFromObject(addr).build();
}


IfAddressBuilder& IfAddressBuilder::loadFromObject(struct rtnl_addr* addr) {
  reset();
  struct nl_addr* ipaddr = rtnl_addr_get_local(addr);
  if (!ipaddr) {
    throw openr::NetlinkException("Failed to get ip address");
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

IfAddressBuilder& IfAddressBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int IfAddressBuilder::getIfIndex() const {
  return ifIndex_;
}

IfAddressBuilder&
IfAddressBuilder::setPrefix(const folly::CIDRNetwork& prefix) {
  prefix_ = prefix;
  return *this;
}

folly::Optional<folly::CIDRNetwork> IfAddressBuilder::getPrefix() const {
  return prefix_;
}

IfAddressBuilder& IfAddressBuilder::setFamily(uint8_t family) {
  family_ = family;
  return *this;
}

// Family will be shadowed if prefix is set
folly::Optional<uint8_t> IfAddressBuilder::getFamily() const {
  return family_;
}

IfAddressBuilder& IfAddressBuilder::setScope(uint8_t scope) {
  scope_ = scope;
  return *this;
}

folly::Optional<uint8_t> IfAddressBuilder::getScope() const {
  return scope_;
}

IfAddressBuilder& IfAddressBuilder::setFlags(uint8_t flags) {
  flags_ = flags;
  return *this;
}

folly::Optional<uint8_t> IfAddressBuilder::getFlags() const {
  return flags_;
}

IfAddressBuilder& IfAddressBuilder::setValid(bool isValid) {
  isValid_ = isValid;
  return *this;
}

bool IfAddressBuilder::isValid() const {
  return isValid_;
}

void IfAddressBuilder::reset() {
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
    family_(builder.getFamily()) {
  init();
}

IfAddress::~IfAddress() {
  if (ifAddr_) {
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  }
}

IfAddress::IfAddress(IfAddress&& other) noexcept
  : prefix_(other.prefix_),
    ifIndex_(other.ifIndex_),
    isValid_(other.isValid_),
    scope_(other.scope_),
    flags_(other.flags_),
    family_(other.family_) {
  if (other.ifAddr_) {
    ifAddr_ = other.ifAddr_;
    other.ifAddr_ = nullptr;
  }
}

IfAddress& IfAddress::operator=(IfAddress&& other) noexcept {
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
  if (other.ifAddr_) {
    ifAddr_ = other.ifAddr_;
    other.ifAddr_ = nullptr;
  }
  return *this;
}

uint8_t IfAddress::getFamily() const {
  if (prefix_.hasValue()) {
    return prefix_->first.family();
  } else {
    return family_.value();
  }
}

uint8_t IfAddress::getPrefixLen() const {
  if (prefix_.hasValue()) {
    return prefix_->second;
  }
  return 0;
}

int IfAddress::getIfIndex() const {
  return ifIndex_;
}

bool IfAddress::isValid() const {
  return isValid_;
}

folly::Optional<folly::CIDRNetwork> IfAddress::getPrefix() const {
  return prefix_;
}

folly::Optional<uint8_t> IfAddress::getScope() const {
  return scope_;
}

folly::Optional<uint8_t> IfAddress::getFlags() const {
  return flags_;
}

// Will construct rtnl_addr object on the first time call, then will return
// the same object pointer
struct rtnl_addr* IfAddress::fromIfAddress() const {
  return ifAddr_;
}

void IfAddress::init() {
  if (ifAddr_) {
    return;
  }

  ifAddr_ = rtnl_addr_alloc();
  if (nullptr == ifAddr_) {
    throw NetlinkException("Failed to create rtnl_addr object");
  }
  rtnl_addr_set_ifindex(ifAddr_, ifIndex_);

  // Get local addr
  struct nl_addr* localAddr = nullptr;
  if (prefix_.hasValue()) {
    localAddr = nl_addr_build(
      prefix_->first.family(),
      (void*)(prefix_->first.bytes()),
      prefix_->first.byteCount());
    if (nullptr == localAddr) {
      throw NetlinkException("Failed to create local addr");
    }
    nl_addr_set_prefixlen(localAddr, prefix_->second);
    // Setting the local address will automatically set the address family
    // and the prefix length to the correct values.
    rtnl_addr_set_local(ifAddr_, localAddr);
  }

  // rtnl_addr_set_local will increase reference for localAddr
  SCOPE_EXIT {
    if (localAddr) {
      nl_addr_put(localAddr);
    }
  };

  SCOPE_FAIL {
    if (localAddr) {
      nl_addr_put(localAddr);
    }
    rtnl_addr_put(ifAddr_);
    ifAddr_ = nullptr;
  };

  if (family_.hasValue()) {
    rtnl_addr_set_family(ifAddr_, family_.value());
  }
  if (scope_.hasValue()) {
    rtnl_addr_set_scope(ifAddr_, scope_.value());
  }
  if (flags_.hasValue()) {
    rtnl_addr_set_flags(ifAddr_, flags_.value());
  }
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
    throw openr::NetlinkException(
        "Failed to get destination IP from neighbor entry");
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
      throw openr::NetlinkException(
          "Failed to get link address from neighbor entry");
    }
    // Skip entries with invalid mac-addresses
    if (nl_addr_get_len(linkAddress) != folly::MacAddress::SIZE) {
      LOG(ERROR) << "Invalid link address for neigbbor";
      throw openr::NetlinkException("Invalid mac address");
    }
    macAddress = folly::MacAddress::fromBinary(folly::ByteRange(
        static_cast<const unsigned char*>(nl_addr_get_binary_addr(linkAddress)),
        nl_addr_get_len(linkAddress)));
  }
  builder.setLinkAddress(macAddress);

  std::array<char, 128> stateBuf = {""};
  VLOG(4)
      << "Built neighbor entry: "
      << " family " << rtnl_neigh_get_family(neighbor)
      << " IfIndex " << rtnl_neigh_get_ifindex(neighbor) << " : "
      << ipAddress.str() << " -> " << macAddress.toString() << " state "
      << rtnl_neigh_state2str(state, stateBuf.data(), stateBuf.size());

  return builder.build();
}

Neighbor NeighborBuilder::build() const {
  return Neighbor(*this);
}

NeighborBuilder& NeighborBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int NeighborBuilder::getIfIndex() const {
  return ifIndex_;
}

NeighborBuilder& NeighborBuilder::setDestination(const folly::IPAddress& dest) {
  destination_ = dest;
  return *this;
}

folly::IPAddress NeighborBuilder::getDestination() const {
  return destination_;
}

NeighborBuilder& NeighborBuilder::setLinkAddress(
  const folly::MacAddress& linkAddress) {
    linkAddress_ = linkAddress;
    return *this;
}

folly::Optional<folly::MacAddress> NeighborBuilder::getLinkAddress() const {
  return linkAddress_;
}

NeighborBuilder& NeighborBuilder::setState(int state, bool deleted) {
  state_ = state;
  isReachable_ = deleted ? false : isNeighborReachable(state);
  return *this;
}

folly::Optional<int> NeighborBuilder::getState() const {
  return state_;
}

bool NeighborBuilder::getIsReachable() const {
  return isReachable_;
}

Neighbor::Neighbor(const NeighborBuilder& builder)
  : ifIndex_(builder.getIfIndex()),
    isReachable_(builder.getIsReachable()),
    destination_(builder.getDestination()),
    linkAddress_(builder.getLinkAddress()),
    state_(builder.getState()) {
  init();
}

Neighbor::~Neighbor() {
  if (neigh_) {
    rtnl_neigh_put(neigh_);
    neigh_ = nullptr;
  }
}

Neighbor::Neighbor(Neighbor&& other) noexcept
  : ifIndex_(other.ifIndex_),
    isReachable_(other.isReachable_),
    destination_(other.destination_),
    linkAddress_(other.linkAddress_),
    state_(other.state_) {
  if (other.neigh_) {
    neigh_ = other.neigh_;
    other.neigh_ = nullptr;
  }
}

Neighbor& Neighbor::operator=(Neighbor && other) noexcept {
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

int Neighbor::getIfIndex() const {
  return ifIndex_;
}

int Neighbor::getFamily() const {
  return destination_.family();
}

folly::IPAddress Neighbor::getDestination() const {
  return destination_;
}

folly::Optional<folly::MacAddress> Neighbor::getLinkAddress() const {
  return linkAddress_;
}

folly::Optional<int> Neighbor::getState() const {
  return state_;
}

bool Neighbor::isReachable() const {
  return isReachable_;
}

struct rtnl_neigh* Neighbor::fromNeighbor() const {
  return neigh_;
}

void Neighbor::init() {
  neigh_ = rtnl_neigh_alloc();
  if (!neigh_) {
    throw NetlinkException("create neighbor object failed");
  }
  rtnl_neigh_set_ifindex(neigh_, ifIndex_);

  struct nl_addr* dst =
    nl_addr_build(destination_.family(),
                  (void*)(destination_.bytes()),
                  destination_.byteCount());
  if (dst == nullptr) {
    throw NetlinkException("Failed to create dst addr");
  }
  rtnl_neigh_set_dst(neigh_, dst);

  struct nl_addr* llAddr = nullptr;
  if (linkAddress_.hasValue()) {
    llAddr = nl_addr_build(
                  AF_UNSPEC,
                  (void*)linkAddress_.value().bytes(),
                  folly::MacAddress::SIZE);
    if (llAddr == nullptr) {
      throw NetlinkException("Failed to create link addr");
    }
    rtnl_neigh_set_lladdr(neigh_, llAddr);
  }

  // neigh object takes a ref if dst/llAddr is successfully set
  // Either way, success or failure, we drop our ref
  SCOPE_EXIT {
    nl_addr_put(dst);
    if (llAddr) {
      nl_addr_put(llAddr);
    }
  };

  SCOPE_FAIL {
    if (dst) {
      nl_addr_put(dst);
    }
    if (llAddr) {
      nl_addr_put(llAddr);
    }
  };

  if (state_.hasValue()) {
    rtnl_neigh_set_state(neigh_, state_.value());
  }
}

/*==================================Link======================================*/

Link LinkBuilder::buildFromObject(struct rtnl_link* link) const {
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

Link LinkBuilder::build() const {
  return Link(*this);
}

LinkBuilder& LinkBuilder::setLinkName(const std::string& linkName) {
  linkName_ = linkName;
  return *this;
}

const std::string& LinkBuilder::getLinkName() const {
  return linkName_;
}

LinkBuilder& LinkBuilder::setIfIndex(int ifIndex) {
  ifIndex_ = ifIndex;
  return *this;
}

int LinkBuilder::getIfIndex() const {
  return ifIndex_;
}

LinkBuilder& LinkBuilder::setFlags(uint32_t flags) {
  flags_ = flags;
  return *this;
}

uint32_t LinkBuilder::getFlags() const {
  return flags_;
}

Link::Link(const LinkBuilder& builder) {
  ifIndex_ = builder.getIfIndex();
  linkName_ = builder.getLinkName();
  flags_ = builder.getFlags();
  init();
}

Link::~Link() {
  if (link_) {
    rtnl_link_put(link_);
    link_ = nullptr;
  }
}

Link::Link(Link&& other) noexcept
  : linkName_(other.linkName_),
    ifIndex_(other.ifIndex_),
    flags_(other.flags_) {
  if (other.link_) {
    link_ = other.link_;
    other.link_ = nullptr;
  }
}

Link& Link::operator=(Link&& other) noexcept {
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
  if (other.link_) {
    link_ = other.link_;
    other.link_ = nullptr;
  }
  return *this;
}

const std::string& Link::getLinkName() const {
  return linkName_;
}

int Link::getIfIndex() const {
  return ifIndex_;
}

uint32_t Link::getFlags() const {
  return flags_;
}

void Link::init() {
  link_ = rtnl_link_alloc();
  if (!link_) {
    throw NetlinkException("Allocate link object failed");
  }
  rtnl_link_set_ifindex(link_, ifIndex_);
  rtnl_link_set_flags(link_, flags_);
  rtnl_link_set_name(link_, linkName_.c_str());
}

bool Link::isUp() const {
  return !!(flags_ & IFF_RUNNING);
}

struct rtnl_link* Link::fromLink() const {
  return link_;
}

} // namespace fbnl
} // namespace openr
