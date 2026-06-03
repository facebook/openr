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
void
Session::onTimerTick() {
  /*
   * TODO: drive the periodic fake-key-version bump. Check whether
   * fakeKeyVersionBumpIntervalSec has elapsed since lastFakeKeyBumpSec_ and, if
   * so, call bumpFakeKeys(). See ScaleTestServer.cpp:915-947 for the legacy
   * behavior being ported.
   */
}
void
Session::bumpFakeKeys() {
  /*
   * TODO: implement the periodic fake-key-version bump (legacy parity with
   * ScaleTestServer.cpp:915-947):
   *   1. ++fakeKeyVersion_
   *   2. regenerate fake keys for every router via
   *      KvStoreThriftInjector::createFakeKeyValues(router,
   *      numFakeKeysPerNode, fakeKeyVersion_)
   *   3. push to BOTH sinks so their versions stay in sync:
   *      injector_->injectKeyVals(...) (the DUT) AND
   *      kvManager_->propagateKeyUpdates(...) (the fake neighbor KvStores)
   * Scheduling note: prefer a folly::AsyncTimeout on the injector's EventBase
   * (the bump issues a Thrift RPC) over a FunctionScheduler polling thread.
   */
}

} // namespace openr
