/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/Session.h>

#include <stdexcept>

#include <openr/tests/scale/DutPatcher.h>
#include <openr/tests/scale/KvStoreDataBuilder.h>
#include <openr/tests/scale/NetInterfaceUtils.h>
#include <openr/tests/scale/SparkNeighborDistribution.h>
#include <openr/tests/scale/TopologyFactory.h>

#include <fmt/format.h>

namespace openr {

namespace {

constexpr int kFakeKvStoreIoThreads = 32;

/*
 * Throws thrift::SetupError if any topology-independent field of
 * ScaleTestConfig required by the Session runtime is unset. All scale knobs and
 * side-effecting bools are optional in the IDL so the server can distinguish
 * "unset" from zero/false and reject both. Topology-specific fields (type,
 * node counts, ecmpWidth, ...) are validated by createScaleTopology().
 */
void
validateConfig(const thrift::ScaleTestConfig& cfg) {
  auto fail = [](std::string msg) {
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
    se.message() = std::move(msg);
    throw se;
  };
  const auto& d = *cfg.dut();
  if (!d.host().has_value() || d.host()->empty()) {
    fail("DutConnection.host must be set and non-empty");
  }
  /*
   * dut.port has an IDL default so it is always present; reject obviously
   * invalid values defensively.
   */
  if (*cfg.dut()->port() <= 0) {
    fail(
        fmt::format(
            "DutConnection.port must be positive (got {})",
            *cfg.dut()->port()));
  }
  // dutRole steers how start() patches the DUT into the built topology.
  if (!cfg.topology()->dutRole().has_value()) {
    fail("TopologyConfig.dutRole must be set");
  }
  const auto& inj = *cfg.injection();
  if (!inj.injectTopology().has_value()) {
    fail("InjectionConfig.injectTopology must be set");
  }
  if (!inj.simulateNeighbors().has_value()) {
    fail("InjectionConfig.simulateNeighbors must be set");
  }
  if (!inj.enableFakeKvStore().has_value()) {
    fail("InjectionConfig.enableFakeKvStore must be set");
  }
}

/*
 * Validates the runtime config then builds the topology via the
 * createScaleTopology seam (implementation selected per build).
 */
Topology
validateAndBuildTopology(const thrift::ScaleTestConfig& cfg) {
  validateConfig(cfg);
  return createScaleTopology(cfg);
}

// Returns true iff `topology_` has a router named `a` with an
// adjacency to `b`. Used to validate downLink/upLink arguments.
bool
hasAdjacency(const Topology& topo, const std::string& a, const std::string& b) {
  auto it = topo.routers.find(a);
  if (it == topo.routers.end()) {
    return false;
  }
  for (const auto& adj : it->second.adjacencies) {
    if (adj.remoteRouterName == b) {
      return true;
    }
  }
  return false;
}

// Links are undirected; sort the endpoint pair so (a,b) and (b,a) hash to
// the same key in downedLinks_.
std::pair<std::string, std::string>
normalizeLinkKey(const std::string& a, const std::string& b) {
  return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

// Neighbors of `name` whose incident link is currently in `downedLinks`. Used
// to rebuild name's adj DB with ALL still-downed incident links omitted, so an
// endpoint with multiple operator-downed links stays symmetric with its peers.
std::set<std::string>
downedNeighborsOf(
    const std::set<std::pair<std::string, std::string>>& downedLinks,
    const std::string& name) {
  std::set<std::string> result;
  for (const auto& [x, y] : downedLinks) {
    if (x == name) {
      result.insert(y);
    } else if (y == name) {
      result.insert(x);
    }
  }
  return result;
}

} // namespace

Session::Session(const thrift::ScaleTestConfig& cfg, int basePortOverride)
    : config_(cfg),
      topology_(validateAndBuildTopology(cfg)),
      startedAt_(std::chrono::steady_clock::now()),
      injector_(
          std::make_unique<KvStoreThriftInjector>(
              *cfg.dut()->host(), static_cast<uint16_t>(*cfg.dut()->port()))),
      dutMonitor_(
          std::make_shared<DutMonitor>(
              *cfg.dut()->host(), static_cast<uint16_t>(*cfg.dut()->port()))),
      kvManager_(
          /*
           * Matches legacy ScaleTestServer.cpp: kvManager is only useful when
           * SparkFaker is also active (the simulated neighbors drive the
           * per-neighbor KvStore servers).
           */
          (cfg.injection()->enableFakeKvStore().value_or(false) &&
           cfg.injection()->simulateNeighbors().value_or(false))
              ? std::make_unique<FakeKvStoreManager>(
                    static_cast<uint16_t>(
                        basePortOverride > 0
                            ? basePortOverride
                            : cfg.injection()->fakeKvStoreBasePort().value_or(
                                  0)),
                    /*ioThreads=*/kFakeKvStoreIoThreads)
              : nullptr),
      sparkIo_(
          cfg.injection()->simulateNeighbors().value_or(false)
              ? std::make_shared<RealSparkIo>()
              : nullptr),
      sparkFaker_(
          sparkIo_ ? std::make_shared<SparkFaker>(
                         std::static_pointer_cast<SparkIoInterface>(sparkIo_))
                   : nullptr),
      scheduler_(std::make_unique<folly::FunctionScheduler>()) {}

Session::~Session() = default;

void
Session::start() {
  validateSparkInterfaces();
  connectToDut();
  const auto dutNeighborNames = patchDut();
  if (kvManager_) {
    setupFakeKvStore(dutNeighborNames);
  }
  if (sparkFaker_) {
    setupSparkFaker(dutNeighborNames);
  }
  injectInitialTopology();
  maybeStartFakeKeyBump();
}

void
Session::validateSparkInterfaces() const {
  /*
   * Validate injection config early so misconfiguration fails fast, before any
   * network I/O.
   */
  if (sparkFaker_ &&
      (!config_.injection()->interfaces().has_value() ||
       config_.injection()->interfaces()->empty())) {
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
    se.message() = "simulateNeighbors=true requires at least one interface";
    throw se;
  }
}

void
Session::connectToDut() {
  auto failDutUnreachable = [&](std::string detail) {
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::DUT_UNREACHABLE;
    se.message() = fmt::format(
        "DUT unreachable ({}:{}): {}",
        *config_.dut()->host(),
        *config_.dut()->port(),
        detail);
    throw se;
  };

  /*
   * KvStoreThriftInjector::connect() and DutMonitor::connect() both return
   * false on failure; some lower-level paths may also throw.
   */
  bool ok = false;
  std::string failMsg;
  try {
    ok = injector_->connect() && dutMonitor_->connect();
  } catch (const std::exception& ex) {
    failMsg = ex.what();
  }
  if (!ok) {
    failDutUnreachable(
        failMsg.empty() ? "connect() returned false"
                        : fmt::format("connect() threw: {}", failMsg));
  }
  dutNodeName_ = injector_->getDutNodeName();
  if (dutNodeName_.empty()) {
    failDutUnreachable("getDutNodeName() returned empty after connect");
  }
  XLOGF(INFO, "[Session] Connected to DUT: {}", dutNodeName_);
}

std::vector<std::string>
Session::patchDut() {
  /*
   * Patch the DUT into topology_. This is the only mutation of topology_ in the
   * Session lifecycle.
   */
  const bool dutIsSpine =
      (*config_.topology()->dutRole() == thrift::DutRole::SPINE);
  if (!dutIsSpine) {
    DutPatcher::stripReplacedLeaf(topology_);
  }
  auto dutNeighborNames = DutPatcher::buildDutNeighborNames(config_);
  DutPatcher::patchDutIntoTopology(
      topology_,
      dutNodeName_,
      dutNeighborNames,
      config_.injection()->interfaces().value_or(std::vector<std::string>{}));
  XLOGF(
      INFO,
      "[Session] topology_: {} routers, {} adjacencies after DUT patch",
      topology_.getRouterCount(),
      topology_.getTotalAdjacencyCount());
  return dutNeighborNames;
}

void
Session::setupFakeKvStore(const std::vector<std::string>& dutNeighborNames) {
  /*
   * The fake KvStore neighbor set must match the simulated topology: every DUT
   * neighbor (from buildDutNeighborNames, which uses the BBF leaf-N / spine-N
   * naming scheme) must be a real router in topology_. Generic topologies
   * (fabric/ring/grid) name nodes differently, so a topology-type vs.
   * neighbor-scheme mismatch would otherwise spin up phantom servers for nodes
   * that patchDutIntoTopology already WARN-skipped. Fail loudly here. Thrown
   * before the try below so it surfaces as TOPOLOGY_INVALID rather than being
   * remapped to INTERNAL.
   */
  if (auto missing = DutPatcher::missingNeighbors(topology_, dutNeighborNames);
      !missing.empty()) {
    std::string joined;
    for (const auto& name : missing) {
      if (!joined.empty()) {
        joined += ", ";
      }
      joined += name;
    }
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
    se.message() = fmt::format(
        "FakeKvStoreManager: {} DUT neighbor(s) absent from topology type "
        "'{}' (neighbor-name scheme mismatch): {}",
        missing.size(),
        *config_.topology()->type(),
        joined);
    throw se;
  }

  /*
   * One per-neighbor Thrift server backed by shared immutable KV data. The
   * catch only handles synchronous failures (e.g. addNeighbor throwing);
   * per-neighbor port-bind happens in threads inside
   * FakeKvStoreManager::start() and is logged there, not surfaced here.
   */
  try {
    auto allKeyVals = KvStoreThriftInjector::buildKeyVals(
        topology_, config_.injection()->numFakeKeysPerNode().value_or(0));
    /*
     * LinkMonitor owns adj:<dut> on the real DUT; injecting it from the test
     * side would conflict with the DUT's own writes.
     */
    allKeyVals.erase(fmt::format("adj:{}", dutNodeName_));
    auto sharedKeyVals =
        std::make_shared<const thrift::KeyVals>(std::move(allKeyVals));
    for (const auto& name : dutNeighborNames) {
      kvManager_->addNeighbor(name, sharedKeyVals);
    }
    kvManager_->start();
  } catch (const std::exception& ex) {
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::INTERNAL;
    se.message() =
        fmt::format("FakeKvStoreManager setup failed: {}", ex.what());
    throw se;
  }
  XLOGF(
      INFO,
      "[Session] FakeKvStoreManager started with {} neighbors",
      kvManager_->getNeighborCount());
}

void
Session::setupSparkFaker(const std::vector<std::string>& dutNeighborNames) {
  /*
   * SparkFaker: send real UDP Spark packets so the DUT believes it has live
   * neighbors, then delegate the neighbor->interface distribution to
   * distributeSparkNeighbors().
   *
   * Every configured interface MUST be usable (resolvable ifIndex + a
   * link-local source address); we fail hard rather than skipping unusable
   * ones. This keeps the Spark distribution on the exact same interface set
   * that DutPatcher::patchDutIntoTopology used for the neighbor->DUT
   * adjacencies — otherwise the injected topology and the real Spark packets
   * would disagree on which interface each neighbor lives on. It also avoids
   * silently starting SparkFaker with a degraded interface set.
   */
  std::vector<UsableInterface> usable;
  usable.reserve(config_.injection()->interfaces()->size());
  for (const auto& ifName : *config_.injection()->interfaces()) {
    const int ifIndex = resolveIfIndex(ifName);
    if (ifIndex < 0) {
      thrift::SetupError se;
      se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
      se.message() = fmt::format(
          "Interface {} not found (no ifIndex) — cannot simulate neighbors",
          ifName);
      throw se;
    }
    auto addrs = lookupLinkLocalAddrs(ifName);
    if (addrs.empty()) {
      thrift::SetupError se;
      se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
      se.message() = fmt::format(
          "Interface {} has no link-local address — cannot simulate neighbors",
          ifName);
      throw se;
    }
    usable.push_back(
        {ifName, ifIndex, addrs.front(), ipv4FromVlanIfName(ifName)});
  }

  for (const auto& iface : usable) {
    sparkIo_->addInterface(iface.ifName, iface.ifIndex);
  }
  for (const auto& placement :
       distributeSparkNeighbors(usable, dutNeighborNames)) {
    sparkFaker_->addNeighbor(
        placement.neighborName,
        placement.neighborIfName,
        placement.hostIfIndex,
        placement.v6Addr,
        placement.hostIfName,
        placement.hostIfIndex,
        placement.v4Addr);
    if (kvManager_) {
      sparkFaker_->setNeighborCtrlPort(
          placement.neighborName, kvManager_->getPort(placement.neighborName));
    }
  }
  sparkIo_->startReceiving();
  /*
   * RealSparkIo::startReceiving() logs and skips interfaces whose receive
   * socket fails to bind / join multicast, so a positive neighbor count alone
   * does not mean the DUT is reachable. Require one started receiver per unique
   * host ifIndex; otherwise the simulated neighbors would be silently
   * nonfunctional.
   */
  std::set<int> expectedIfIndexes;
  for (const auto& iface : usable) {
    expectedIfIndexes.insert(iface.ifIndex);
  }
  if (const size_t started = sparkIo_->numActiveReceivers();
      started < expectedIfIndexes.size()) {
    thrift::SetupError se;
    se.reason() = thrift::SetupErrorReason::INTERNAL;
    se.message() = fmt::format(
        "SparkFaker: only {} of {} interface receive sockets started "
        "(bind/multicast-join failure — see [REAL-SPARK-IO] logs)",
        started,
        expectedIfIndexes.size());
    throw se;
  }
  sparkFaker_->start();
  XLOGF(
      INFO,
      "[Session] SparkFaker started with {} neighbors across {} interfaces",
      sparkFaker_->getNeighborCount(),
      usable.size());
}

void
Session::injectInitialTopology() {
  /*
   * Initial bulk topology injection into the DUT's KvStore. The DUT's own adj
   * key is owned by LinkMonitor and must be omitted.
   */
  if (*config_.injection()->injectTopology()) {
    try {
      auto keyVals = KvStoreThriftInjector::buildKeyVals(
          topology_, config_.injection()->numFakeKeysPerNode().value_or(0));
      keyVals.erase(fmt::format("adj:{}", dutNodeName_));
      const size_t expected = keyVals.size();
      const size_t injected = injector_->injectKeyVals(keyVals);
      /*
       * injectKeyVals() returns 0 (it does NOT throw) when the DUT is
       * disconnected or the setKvStoreKeyVals RPC fails, so the catch below is
       * not enough on its own. Treat a short write as an explicit failure
       * rather than letting start() report a healthy session whose DUT never
       * received the initial topology.
       */
      if (injected != expected) {
        thrift::SetupError se;
        se.reason() = thrift::SetupErrorReason::INJECTION_FAILED;
        se.message() = fmt::format(
            "Topology injection incomplete: injected {} of {} keys into DUT "
            "KvStore (DUT disconnected or setKvStoreKeyVals RPC failed)",
            injected,
            expected);
        throw se;
      }
      XLOGF(INFO, "[Session] Injected {} keys into DUT KvStore", injected);
    } catch (const thrift::SetupError&) {
      throw; // already classified; don't re-wrap as a generic failure
    } catch (const std::exception& ex) {
      thrift::SetupError se;
      se.reason() = thrift::SetupErrorReason::INJECTION_FAILED;
      se.message() = fmt::format("Topology injection failed: {}", ex.what());
      throw se;
    }
  }
}

std::vector<std::string>
Session::listNodesUnlocked() const {
  std::vector<std::string> out;
  out.reserve(topology_.routers.size());
  for (const auto& [name, _] : topology_.routers) {
    if (name != dutNodeName_) {
      out.push_back(name);
    }
  }
  return out;
}

std::vector<std::string>
Session::listNodes() const {
  std::lock_guard<std::mutex> g(mutationMutex_);
  return listNodesUnlocked();
}

void
Session::downNode(const std::string& name) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  /*
   * The downNode/upNode IDL only declares UnknownNodeError and NotRunningError,
   * so UnknownNodeError is deliberately overloaded below to also mean "node is
   * known but cannot be acted on" (it is the DUT, or it is already in the
   * requested state). The message disambiguates; add a dedicated NodeStateError
   * to the IDL if clients ever need to distinguish these programmatically.
   */
  if (name == dutNodeName_) {
    thrift::UnknownNodeError e;
    e.message() = "cannot manipulate the DUT directly";
    throw e;
  }
  if (topology_.routers.count(name) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", name);
    throw e;
  }
  if (downedNodes_.count(name)) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Node {} is already down", name);
    throw e;
  }

  /*
   * Operator downLink() intent persists across a node flap: we do NOT clear
   * incident downedLinks_ entries here. While the node is down they are hidden
   * from getStatus() (subsumed by the downed node); when the node is restored,
   * upNode() rebuilds its adjacencies with the still-downed links omitted.
   */

  if (sparkFaker_ && sparkFaker_->failNeighbor(name)) {
    XLOGF(INFO, "[CMD] Spark: failed neighbor {}", name);
  }

  const int64_t removeVersion = cmdVersion_.fetch_add(1) + 1;

  /*
   * Remove the downed node's own adj DB. Pass removeVersion so the fake KvStore
   * removal shares cmdVersion_ with the matching upNode restore; otherwise the
   * two would run on separate version streams and a later removal could lose
   * KvStore version arbitration on repeated down/up flaps.
   */
  if (kvManager_) {
    kvManager_->simulateNodeRemoval(name, removeVersion);
  }
  if (injector_ && injector_->isConnected()) {
    injector_->removeNode(name, removeVersion);
  }
  XLOGF(INFO, "[CMD] KvStore: removed adj:{}", name);

  /*
   * Update adj DBs of all neighbors of the downed node to drop their
   * adjacency to it. In reality, those neighbors would detect the link
   * failure and update their own adj DBs.
   */
  const auto& downedRouter = topology_.getRouter(name);
  thrift::KeyVals neighborKeyVals;
  for (const auto& adj : downedRouter.adjacencies) {
    if (topology_.routers.count(adj.remoteRouterName) == 0) {
      continue;
    }
    if (adj.remoteRouterName == dutNodeName_) {
      continue; // we don't manage the DUT's adj DB
    }
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    // Drop this neighbor's adjacency to the downed node AND keep any of its
    // OTHER operator-downed links omitted — otherwise rebuilding it here would
    // silently resurrect those links.
    auto omit = downedNeighborsOf(downedLinks_, adj.remoteRouterName);
    omit.insert(name);
    auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
        topology_.getRouter(adj.remoteRouterName), topology_, omit, v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    neighborKeyVals.emplace(std::move(key), std::move(value));
  }
  if (injector_ && injector_->isConnected() && !neighborKeyVals.empty()) {
    injector_->injectKeyVals(neighborKeyVals);
    XLOGF(
        INFO,
        "[CMD] KvStore: updated {} neighbor adj DBs",
        neighborKeyVals.size());
  }

  downedNodes_.insert(name);
  XLOGF(INFO, "[CMD] {} is now DOWN", name);
}

void
Session::upNode(const std::string& name) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  /*
   * UnknownNodeError is overloaded for DUT / wrong-state cases here too; see
   * the rationale in downNode().
   */
  if (name == dutNodeName_) {
    thrift::UnknownNodeError e;
    e.message() = "cannot manipulate the DUT directly";
    throw e;
  }
  if (topology_.routers.count(name) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", name);
    throw e;
  }
  if (!downedNodes_.count(name)) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Node {} is not down", name);
    throw e;
  }

  if (sparkFaker_ && sparkFaker_->recoverNeighbor(name)) {
    XLOGF(INFO, "[CMD] Spark: recovered neighbor {}", name);
  }

  /*
   * Operator downLink() intent persists across the node flap: any link still in
   * downedLinks_ that is incident on this node must remain DOWN after the node
   * is restored. Restore the node's adj DB with those still-downed neighbors
   * omitted, and for each such neighbor restore its adj DB with this node
   * omitted; neighbors whose link is up get a full restore.
   */
  const std::set<std::string> stillDownNeighbors =
      downedNeighborsOf(downedLinks_, name);

  thrift::KeyVals allKeyVals;
  {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    auto [key, value] = stillDownNeighbors.empty()
        ? KvStoreDataBuilder::buildAdjKeyValue(
              topology_.getRouter(name), topology_, v)
        : KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
              topology_.getRouter(name), topology_, stillDownNeighbors, v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    allKeyVals.emplace(std::move(key), std::move(value));
  }
  XLOGF(INFO, "[CMD] KvStore: restored adj:{}", name);

  // Restore adj DBs of all neighbors to include the recovered node, while
  // keeping every neighbor's own still-downed links omitted (including the link
  // back to `name` if it is itself still operator-downed).
  const auto& recoveredRouter = topology_.getRouter(name);
  for (const auto& adj : recoveredRouter.adjacencies) {
    if (topology_.routers.count(adj.remoteRouterName) == 0) {
      continue;
    }
    if (adj.remoteRouterName == dutNodeName_) {
      continue; // we don't manage the DUT's adj DB
    }
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    // Rebuild this neighbor omitting ALL of ITS still-downed links (this set
    // already includes `name` when the link to it remains down). Using only the
    // link to `name` would silently resurrect the neighbor's other downed
    // links.
    auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
        topology_.getRouter(adj.remoteRouterName),
        topology_,
        downedNeighborsOf(downedLinks_, adj.remoteRouterName),
        v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    allKeyVals.emplace(std::move(key), std::move(value));
  }
  if (injector_ && injector_->isConnected()) {
    injector_->injectKeyVals(allKeyVals);
    XLOGF(
        INFO,
        "[CMD] KvStore: updated {} adj DBs (node + neighbors)",
        allKeyVals.size());
  }

  // downedLinks_ entries incident on this node are intentionally NOT cleared:
  // operator link-down intent persists across the node flap (the links above
  // were rebuilt as still-down). Only the node itself is marked up.
  downedNodes_.erase(name);
  XLOGF(INFO, "[CMD] {} is now UP", name);
}

void
Session::downLink(const std::string& a, const std::string& b) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  if (a == dutNodeName_ || b == dutNodeName_) {
    thrift::UnknownNodeError e;
    e.message() = "cannot manipulate links to the DUT directly";
    throw e;
  }
  // Drive validation from topology_ — the canonical, never-mutated
  // source of truth. Unknown endpoint nodes throw UnknownNodeError; missing
  // adjacency or already-down state throws UnknownAdjacencyError.
  if (topology_.routers.count(a) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", a);
    throw e;
  }
  if (topology_.routers.count(b) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", b);
    throw e;
  }
  if (!hasAdjacency(topology_, a, b)) {
    thrift::UnknownAdjacencyError e;
    e.message() = fmt::format("No adjacency between {} and {}", a, b);
    throw e;
  }
  if (downedNodes_.count(a) || downedNodes_.count(b)) {
    thrift::UnknownAdjacencyError e;
    e.message() =
        fmt::format("endpoint already down ({} or {} in downedNodes_)", a, b);
    throw e;
  }
  const auto link = normalizeLinkKey(a, b);
  if (downedLinks_.count(link)) {
    thrift::UnknownAdjacencyError e;
    e.message() = fmt::format("link {}<->{} already down", a, b);
    throw e;
  }

  // Record in downedLinks_ BEFORE building the keys so each endpoint's adj DB
  // omits ALL of its still-downed incident links (not just this one) — keeping
  // both sides symmetric when an endpoint already has other operator-downed
  // links. Also lets dtor / external observers see the in-flight state.
  downedLinks_.insert(link);

  // One cmdVersion bump per key for monotonicity in the DUT's KvStore. Note:
  // fetch_add returns the prior value; +1 gives the post-increment value,
  // matching the monotonic semantics used by downNode/upNode.
  const int64_t vA = cmdVersion_.fetch_add(1) + 1;
  const int64_t vB = cmdVersion_.fetch_add(1) + 1;
  auto keyA = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topology_.getRouter(a),
      topology_,
      downedNeighborsOf(downedLinks_, a),
      vA);
  auto keyB = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topology_.getRouter(b),
      topology_,
      downedNeighborsOf(downedLinks_, b),
      vB);

  try {
    thrift::KeyVals kv;
    kv.emplace(keyA.first, keyA.second);
    kv.emplace(keyB.first, keyB.second);
    if (injector_ && injector_->isConnected()) {
      const size_t injected = injector_->injectKeyVals(kv);
      if (injected != kv.size()) {
        /*
         * injectKeyVals() returns a short count (it does not throw) on
         * disconnect / RPC failure. Turn a short write into a hard failure so a
         * half-applied, asymmetric link state on the DUT is never reported as
         * success. Caught below to trigger rollback.
         */
        throw std::runtime_error(
            fmt::format(
                "downLink injection incomplete: {} of {} keys written to DUT",
                injected,
                kv.size()));
      }
    }
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(keyA.first, keyA.second);
      kvManager_->propagateKeyUpdate(keyB.first, keyB.second);
    }
    XLOGF(INFO, "[CMD] Link {}<->{} is now DOWN", a, b);
  } catch (...) {
    // Best-effort rollback to BOTH sinks: re-push the full adj keys for both
    // endpoints to the DUT and the fake KvStore, and drop the in-memory
    // record. If the rollback push also fails, swallow — stopTest+startTest
    // provides a clean reset.
    downedLinks_.erase(link);
    try {
      // Rebuild with each endpoint's REMAINING still-downed links preserved
      // (downedNeighborsOf now excludes the just-erased link), so a failed
      // downLink does not clobber other operator-downed links on a or b.
      const int64_t rvA = cmdVersion_.fetch_add(1) + 1;
      const int64_t rvB = cmdVersion_.fetch_add(1) + 1;
      auto rollbackA = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
          topology_.getRouter(a),
          topology_,
          downedNeighborsOf(downedLinks_, a),
          rvA);
      auto rollbackB = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
          topology_.getRouter(b),
          topology_,
          downedNeighborsOf(downedLinks_, b),
          rvB);
      thrift::KeyVals rollbackKv;
      rollbackKv.emplace(rollbackA.first, rollbackA.second);
      rollbackKv.emplace(rollbackB.first, rollbackB.second);
      if (injector_ && injector_->isConnected()) {
        injector_->injectKeyVals(rollbackKv);
      }
      if (kvManager_) {
        kvManager_->propagateKeyUpdate(rollbackA.first, rollbackA.second);
        kvManager_->propagateKeyUpdate(rollbackB.first, rollbackB.second);
      }
    } catch (...) {
      // Swallow rollback failures.
    }
    throw;
  }
}

void
Session::upLink(const std::string& a, const std::string& b) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  if (a == dutNodeName_ || b == dutNodeName_) {
    thrift::UnknownNodeError e;
    e.message() = "cannot manipulate links to the DUT directly";
    throw e;
  }
  if (topology_.routers.count(a) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", a);
    throw e;
  }
  if (topology_.routers.count(b) == 0) {
    thrift::UnknownNodeError e;
    e.message() = fmt::format("Unknown node: {}", b);
    throw e;
  }
  if (!hasAdjacency(topology_, a, b)) {
    thrift::UnknownAdjacencyError e;
    e.message() = fmt::format("No adjacency between {} and {}", a, b);
    throw e;
  }
  if (downedNodes_.count(a) || downedNodes_.count(b)) {
    thrift::UnknownAdjacencyError e;
    e.message() = fmt::format(
        "endpoint is down ({} or {}); upNode first before upLink", a, b);
    throw e;
  }
  const auto link = normalizeLinkKey(a, b);
  if (!downedLinks_.count(link)) {
    thrift::UnknownAdjacencyError e;
    e.message() = fmt::format("link {}<->{} is not down", a, b);
    throw e;
  }

  // Rebuild both endpoints' adj keys restoring the a<->b edge, but keeping any
  // OTHER still-downed links incident on a or b omitted (so bringing one link
  // up doesn't silently resurrect the endpoint's other operator-downed links).
  // downedNeighborsOf still includes the link being brought up, so exclude it.
  auto aStillDown = downedNeighborsOf(downedLinks_, a);
  aStillDown.erase(b);
  auto bStillDown = downedNeighborsOf(downedLinks_, b);
  bStillDown.erase(a);

  // One cmdVersion bump per key for monotonicity in the DUT's KvStore. Note:
  // fetch_add returns the prior value; +1 gives the post-increment value,
  // matching the monotonic semantics used by downNode/upNode.
  const int64_t vA = cmdVersion_.fetch_add(1) + 1;
  const int64_t vB = cmdVersion_.fetch_add(1) + 1;
  auto keyA = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topology_.getRouter(a), topology_, aStillDown, vA);
  auto keyB = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
      topology_.getRouter(b), topology_, bStillDown, vB);

  thrift::KeyVals kv;
  kv.emplace(keyA.first, keyA.second);
  kv.emplace(keyB.first, keyB.second);
  try {
    if (injector_ && injector_->isConnected()) {
      const size_t injected = injector_->injectKeyVals(kv);
      if (injected != kv.size()) {
        /*
         * Short write (injectKeyVals does not throw on RPC failure). Fail
         * loudly so a partially-restored link is not reported as up; caught
         * below to re-assert a consistent down state.
         */
        throw std::runtime_error(
            fmt::format(
                "upLink injection incomplete: {} of {} keys written to DUT",
                injected,
                kv.size()));
      }
    }
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(keyA.first, keyA.second);
      kvManager_->propagateKeyUpdate(keyB.first, keyB.second);
    }
    // Erase from downedLinks_ only after a successful push.
    downedLinks_.erase(link);
    XLOGF(INFO, "[CMD] Link {}<->{} is now UP", a, b);
  } catch (...) {
    /*
     * Best-effort rollback to a CONSISTENT down state. The link stays recorded
     * as down (downedLinks_ was NOT erased), so a partial write that brought
     * one endpoint up would leave the DUT half-restored and unreconcilable with
     * the session view. Re-assert the link-down adj keys on BOTH endpoints to
     * both sinks (downedNeighborsOf still includes the peer, since the link is
     * still recorded down). Symmetric with downLink's rollback; swallow
     * rollback failures — stopTest+startTest provides a clean reset.
     */
    try {
      const int64_t rvA = cmdVersion_.fetch_add(1) + 1;
      const int64_t rvB = cmdVersion_.fetch_add(1) + 1;
      auto downA = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
          topology_.getRouter(a),
          topology_,
          downedNeighborsOf(downedLinks_, a),
          rvA);
      auto downB = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
          topology_.getRouter(b),
          topology_,
          downedNeighborsOf(downedLinks_, b),
          rvB);
      thrift::KeyVals rollbackKv;
      rollbackKv.emplace(downA.first, downA.second);
      rollbackKv.emplace(downB.first, downB.second);
      if (injector_ && injector_->isConnected()) {
        injector_->injectKeyVals(rollbackKv);
      }
      if (kvManager_) {
        kvManager_->propagateKeyUpdate(downA.first, downA.second);
        kvManager_->propagateKeyUpdate(downB.first, downB.second);
      }
    } catch (...) {
      // Swallow rollback failures.
    }
    throw;
  }
}

std::set<std::string>
Session::omitSetFor(const std::string& node) const {
  // Operator-downed links incident on `node` ...
  std::set<std::string> omit = downedNeighborsOf(downedLinks_, node);
  // ... plus any adjacent node that is itself currently downed.
  for (const auto& adj : topology_.getRouter(node).adjacencies) {
    if (downedNodes_.count(adj.remoteRouterName)) {
      omit.insert(adj.remoteRouterName);
    }
  }
  return omit;
}

void
Session::injectAllOrThrow(const thrift::KeyVals& kv, const char* op) {
  if (!injector_ || !injector_->isConnected() || kv.empty()) {
    return;
  }
  const size_t injected = injector_->injectKeyVals(kv);
  if (injected != kv.size()) {
    // Short write (injectKeyVals does not throw on RPC failure). Surface it so
    // a partial bulk write to the DUT is never reported to the operator as
    // success. No rollback for bulk: recover with stopTest + startTest.
    throw std::runtime_error(
        fmt::format(
            "{} injection incomplete: {} of {} keys written to DUT "
            "(no rollback; recover with stopTest + startTest)",
            op,
            injected,
            kv.size()));
  }
}

void
Session::downNodes(const std::vector<std::string>& names) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  // Validate the whole batch up front; reject atomically (nothing applied).
  // std::set both dedups repeated names and gives a stable iteration order.
  std::set<std::string> batch;
  for (const auto& name : names) {
    if (name == dutNodeName_) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("cannot manipulate the DUT directly: {}", name);
      throw e;
    }
    if (topology_.routers.count(name) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", name);
      throw e;
    }
    if (downedNodes_.count(name)) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Node {} is already down", name);
      throw e;
    }
    batch.insert(name);
  }
  if (batch.empty()) {
    return;
  }

  // Apply the new downed state BEFORE rebuilding so omitSetFor() reflects the
  // final set (a neighbor adjacent to several downed nodes omits all of them).
  for (const auto& name : batch) {
    downedNodes_.insert(name);
  }
  // Fail the Spark side for any batch members that are direct DUT neighbors.
  if (sparkFaker_) {
    for (const auto& name : batch) {
      sparkFaker_->failNeighbor(name);
    }
  }
  // Remove each downed node's own adj DB from both sinks.
  for (const auto& name : batch) {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    if (kvManager_) {
      kvManager_->simulateNodeRemoval(name, v);
    }
    if (injector_ && injector_->isConnected()) {
      injector_->removeNode(name, v);
    }
  }
  // Rebuild every still-up neighbor of any downed node exactly once, then
  // inject as a single wave so the DUT processes one convergence event.
  std::set<std::string> affected;
  for (const auto& name : batch) {
    for (const auto& adj : topology_.getRouter(name).adjacencies) {
      const auto& nbr = adj.remoteRouterName;
      if (topology_.routers.count(nbr) == 0 || nbr == dutNodeName_ ||
          downedNodes_.count(nbr)) {
        continue;
      }
      affected.insert(nbr);
    }
  }
  thrift::KeyVals kv;
  for (const auto& nbr : affected) {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
        topology_.getRouter(nbr), topology_, omitSetFor(nbr), v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    kv.emplace(std::move(key), std::move(value));
  }
  injectAllOrThrow(kv, "downNodes");
  XLOGF(
      INFO,
      "[CMD] {} nodes now DOWN ({} neighbor adj DBs updated)",
      batch.size(),
      kv.size());
}

void
Session::upNodes(const std::vector<std::string>& names) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  std::set<std::string> batch;
  for (const auto& name : names) {
    if (name == dutNodeName_) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("cannot manipulate the DUT directly: {}", name);
      throw e;
    }
    if (topology_.routers.count(name) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", name);
      throw e;
    }
    if (!downedNodes_.count(name)) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Node {} is not down", name);
      throw e;
    }
    batch.insert(name);
  }
  if (batch.empty()) {
    return;
  }

  // Restore state first so omitSetFor() no longer omits these nodes. Operator
  // downLink intent persists: links still in downedLinks_ stay omitted below.
  for (const auto& name : batch) {
    downedNodes_.erase(name);
  }
  if (sparkFaker_) {
    for (const auto& name : batch) {
      sparkFaker_->recoverNeighbor(name);
    }
  }
  // Rebuild the restored nodes' own adj DBs AND every still-up neighbor, once
  // each, against the post-restore state.
  std::set<std::string> toBuild(batch.begin(), batch.end());
  for (const auto& name : batch) {
    for (const auto& adj : topology_.getRouter(name).adjacencies) {
      const auto& nbr = adj.remoteRouterName;
      if (topology_.routers.count(nbr) == 0 || nbr == dutNodeName_ ||
          downedNodes_.count(nbr)) {
        continue;
      }
      toBuild.insert(nbr);
    }
  }
  thrift::KeyVals kv;
  for (const auto& node : toBuild) {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    auto omit = omitSetFor(node);
    auto [key, value] = omit.empty()
        ? KvStoreDataBuilder::buildAdjKeyValue(
              topology_.getRouter(node), topology_, v)
        : KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
              topology_.getRouter(node), topology_, omit, v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    kv.emplace(std::move(key), std::move(value));
  }
  injectAllOrThrow(kv, "upNodes");
  XLOGF(
      INFO,
      "[CMD] {} nodes now UP ({} adj DBs updated)",
      batch.size(),
      kv.size());
}

void
Session::downLinks(const std::vector<thrift::LinkRef>& links) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  std::set<std::pair<std::string, std::string>> batch;
  std::set<std::string> endpoints;
  for (const auto& link : links) {
    const auto& a = *link.localNode();
    const auto& b = *link.remoteNode();
    if (a == dutNodeName_ || b == dutNodeName_) {
      thrift::UnknownNodeError e;
      e.message() = "cannot manipulate links to the DUT directly";
      throw e;
    }
    if (topology_.routers.count(a) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", a);
      throw e;
    }
    if (topology_.routers.count(b) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", b);
      throw e;
    }
    if (!hasAdjacency(topology_, a, b)) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format("No adjacency between {} and {}", a, b);
      throw e;
    }
    if (downedNodes_.count(a) || downedNodes_.count(b)) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format("endpoint already down ({} or {})", a, b);
      throw e;
    }
    const auto key = normalizeLinkKey(a, b);
    if (downedLinks_.count(key) || !batch.insert(key).second) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format("link {}<->{} already down / duplicated", a, b);
      throw e;
    }
    endpoints.insert(a);
    endpoints.insert(b);
  }
  if (batch.empty()) {
    return;
  }

  for (const auto& link : batch) {
    downedLinks_.insert(link);
  }
  // Endpoints are all up (validated), so each gets rebuilt once against the
  // final downed-link state and injected as a single wave.
  thrift::KeyVals kv;
  for (const auto& node : endpoints) {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    auto [key, value] = KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
        topology_.getRouter(node), topology_, omitSetFor(node), v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    kv.emplace(std::move(key), std::move(value));
  }
  injectAllOrThrow(kv, "downLinks");
  XLOGF(
      INFO,
      "[CMD] {} links now DOWN ({} adj DBs updated)",
      batch.size(),
      kv.size());
}

void
Session::upLinks(const std::vector<thrift::LinkRef>& links) {
  std::lock_guard<std::mutex> g(mutationMutex_);

  std::set<std::pair<std::string, std::string>> batch;
  std::set<std::string> endpoints;
  for (const auto& link : links) {
    const auto& a = *link.localNode();
    const auto& b = *link.remoteNode();
    if (a == dutNodeName_ || b == dutNodeName_) {
      thrift::UnknownNodeError e;
      e.message() = "cannot manipulate links to the DUT directly";
      throw e;
    }
    if (topology_.routers.count(a) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", a);
      throw e;
    }
    if (topology_.routers.count(b) == 0) {
      thrift::UnknownNodeError e;
      e.message() = fmt::format("Unknown node: {}", b);
      throw e;
    }
    // Mirror singular upLink: a downed-node endpoint must be brought up first.
    if (downedNodes_.count(a) || downedNodes_.count(b)) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format(
          "endpoint is down ({} or {}); upNode first before upLink", a, b);
      throw e;
    }
    const auto key = normalizeLinkKey(a, b);
    if (!downedLinks_.count(key)) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format("link {}<->{} is not down", a, b);
      throw e;
    }
    if (!batch.insert(key).second) {
      thrift::UnknownAdjacencyError e;
      e.message() = fmt::format("duplicate link {}<->{} in request", a, b);
      throw e;
    }
    endpoints.insert(a);
    endpoints.insert(b);
  }
  if (batch.empty()) {
    return;
  }

  for (const auto& link : batch) {
    downedLinks_.erase(link);
  }
  // All endpoints are up; rebuild each once against the post-restore state
  // (other still-downed links/nodes incident on an endpoint stay omitted).
  thrift::KeyVals kv;
  for (const auto& node : endpoints) {
    const int64_t v = cmdVersion_.fetch_add(1) + 1;
    auto omit = omitSetFor(node);
    auto [key, value] = omit.empty()
        ? KvStoreDataBuilder::buildAdjKeyValue(
              topology_.getRouter(node), topology_, v)
        : KvStoreDataBuilder::buildAdjKeyValueWithLinksDown(
              topology_.getRouter(node), topology_, omit, v);
    if (kvManager_) {
      kvManager_->propagateKeyUpdate(key, value);
    }
    kv.emplace(std::move(key), std::move(value));
  }
  injectAllOrThrow(kv, "upLinks");
  XLOGF(
      INFO,
      "[CMD] {} links now UP ({} adj DBs updated)",
      batch.size(),
      kv.size());
}

thrift::TestStatus
Session::getStatus() const {
  std::lock_guard<std::mutex> g(mutationMutex_);
  thrift::TestStatus s;
  s.running() = true;
  s.activeConfig() = config_;
  for (const auto& n : downedNodes_) {
    s.downedNodes()->push_back(n);
  }
  for (const auto& [a, b] : downedLinks_) {
    /*
     * Per the TestStatus contract, links subsumed by a downed node are reported
     * via downedNodes, not duplicated here.
     */
    if (downedNodes_.count(a) || downedNodes_.count(b)) {
      continue;
    }
    thrift::LinkRef lr;
    lr.localNode() = a;
    lr.remoteNode() = b;
    s.downedLinks()->push_back(std::move(lr));
  }
  s.dutConnected() = injector_ && injector_->isConnected();
  s.elapsedSec() = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - startedAt_)
                       .count();
  /*
   * neighborCount is optional and, per the IDL, must stay unset when
   * simulateNeighbors=false (sparkFaker_ is null) so a client can distinguish
   * "0 neighbors" from "neighbor simulation disabled".
   */
  if (sparkFaker_) {
    s.neighborCount() = static_cast<int32_t>(sparkFaker_->getNeighborCount());
  }
  return s;
}
thrift::NeighborStats
Session::getNeighborStats() const {
  std::lock_guard<std::mutex> g(mutationMutex_);
  thrift::NeighborStats out;
  if (!sparkFaker_) {
    // simulateNeighbors=false: all-zero counters, empty neighbor list (the IDL
    // default-initializes the numeric fields to 0).
    return out;
  }
  /*
   * Same data the legacy ScaleTestServer.cpp periodic/final stats dump printed,
   * but structured: aggregate packet counters plus the per-neighbor table.
   */
  const auto& s = sparkFaker_->getStats();
  out.hellosSent() = static_cast<int64_t>(s.hellosSent.load());
  out.hellosReceived() = static_cast<int64_t>(s.hellosReceived.load());
  out.handshakesSent() = static_cast<int64_t>(s.handshakesSent.load());
  out.handshakesReceived() = static_cast<int64_t>(s.handshakesReceived.load());
  out.heartbeatsSent() = static_cast<int64_t>(s.heartbeatsSent.load());
  out.heartbeatsReceived() = static_cast<int64_t>(s.heartbeatsReceived.load());
  out.parseErrors() = static_cast<int64_t>(s.parseErrors.load());
  out.neighborsEstablished() =
      static_cast<int32_t>(s.neighborsEstablished.load());
  out.totalNeighbors() = static_cast<int32_t>(sparkFaker_->getNeighborCount());
  for (auto& v : sparkFaker_->getNeighborViews()) {
    thrift::SparkNeighborState n;
    n.name() = std::move(v.name);
    n.state() = std::move(v.state);
    n.dutNode() = std::move(v.dutNodeName);
    n.failed() = v.failed;
    out.neighbors()->push_back(std::move(n));
  }
  return out;
}
thrift::RouteCounts
Session::verifyRoutes() const {
  /*
   * Hold mutationMutex_ across the RPC: injector_ is shared with the mutation
   * paths (downNode/upNode/down|upLink all call injector_->injectKeyVals), so
   * serializing here avoids concurrent use of the same Thrift client. The query
   * is a single RPC; blocking mutations briefly is acceptable for the
   * single-operator harness.
   */
  std::lock_guard<std::mutex> g(mutationMutex_);
  thrift::RouteCounts rc;
  rc.unicastRoutes() = 0;
  rc.mplsRoutes() = 0;
  if (!injector_ || !injector_->isConnected()) {
    return rc;
  }
  auto routeDb = injector_->getRouteDatabase(dutNodeName_);
  rc.unicastRoutes() = static_cast<int64_t>(routeDb.unicastRoutes()->size());
  rc.mplsRoutes() = static_cast<int64_t>(routeDb.mplsRoutes()->size());
  return rc;
}
thrift::KeyVals
Session::buildFakeKeyVals(int64_t version) const {
  const int32_t numFakeKeys =
      config_.injection()->numFakeKeysPerNode().value_or(0);
  thrift::KeyVals out;
  if (numFakeKeys <= 0) {
    return out;
  }
  /*
   * topology_ is written exactly once during start() and never again, so this
   * read is safe lock-free (the scheduler thread and tests both call here).
   */
  for (const auto& [_, router] : topology_.routers) {
    auto kvs = KvStoreThriftInjector::createFakeKeyValues(
        router, numFakeKeys, version);
    for (auto& [key, value] : kvs) {
      out.emplace(std::move(key), std::move(value));
    }
  }
  return out;
}
void
Session::maybeStartFakeKeyBump() {
  const int32_t numFakeKeys =
      config_.injection()->numFakeKeysPerNode().value_or(0);
  const int32_t intervalSec =
      config_.injection()->fakeKeyVersionBumpIntervalSec().value_or(0);
  if (numFakeKeys <= 0 || intervalSec <= 0) {
    return; // feature disabled
  }
  /*
   * FunctionScheduler fires onTimerTick() on its own thread every intervalSec
   * (no 1s polling like the legacy main loop — the scheduler gives us the exact
   * cadence). scheduler_ is declared last among the runtime members, so it is
   * destroyed FIRST in ~Session(); its dtor joins the worker (waiting for any
   * in-flight bump to finish) before injector_/kvManager_ are torn down, so the
   * callback never touches freed state.
   */
  scheduler_->addFunction(
      [this]() { onTimerTick(); },
      std::chrono::seconds(intervalSec),
      "fakeKeyBump");
  scheduler_->start();
  XLOGF(
      INFO,
      "[bump] periodic fake-key bump enabled: {} keys/node every {}s",
      numFakeKeys,
      intervalSec);
}
void
Session::onTimerTick() {
  // One scheduler tick == one bump (the interval is enforced by the scheduler).
  bumpFakeKeys();
}
void
Session::bumpFakeKeys() {
  const int32_t numFakeKeys =
      config_.injection()->numFakeKeysPerNode().value_or(0);
  if (numFakeKeys <= 0) {
    return;
  }
  const int64_t version = ++fakeKeyVersion_;

  /*
   * Regenerate the fake keys for every router at the new version. Fake keys
   * (fakekeys{i}:{node}) are independent of the adj: keys that down/up mutate,
   * so we intentionally bump ALL routers regardless of downed state — matching
   * the legacy behavior.
   */
  auto fakeKeyVals = buildFakeKeyVals(version);

  /*
   * Push to BOTH sinks so their versions stay aligned. Hold mutationMutex_
   * across the injector_ RPC: the mutation paths (downNode/upNode/down|upLink)
   * also drive injector_->injectKeyVals, and the injector's Thrift client must
   * not be used concurrently from two threads.
   */
  std::lock_guard<std::mutex> g(mutationMutex_);
  if (injector_ && injector_->isConnected()) {
    injector_->injectKeyVals(fakeKeyVals);
  }
  if (kvManager_) {
    kvManager_->propagateKeyUpdates(fakeKeyVals);
  }
  XLOGF(
      INFO,
      "[bump] fake key versions -> {} ({} keys)",
      version,
      fakeKeyVals.size());
}

} // namespace openr
