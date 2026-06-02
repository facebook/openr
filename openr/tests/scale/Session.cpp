/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/Session.h>

#include <openr/tests/scale/DutPatcher.h>
#include <openr/tests/scale/TopologyFactory.h>

#include <fmt/format.h>

namespace openr {

namespace {

constexpr int kFakeKvStoreIoThreads = 32;

// Throws thrift::SetupError if any topology-independent field of
// ScaleTestConfig required by the Session runtime is unset. All scale knobs and
// side-effecting bools are optional in the IDL so the server can distinguish
// "unset" from zero/false and reject both. Topology-specific fields (type,
// node counts, ecmpWidth, ...) are validated by createScaleTopology().
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
  // dut.port has an IDL default so it is always present; reject obviously
  // invalid values defensively.
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

// Validates the runtime config then builds the topology via the
// createScaleTopology seam (implementation selected per build).
Topology
validateAndBuildTopology(const thrift::ScaleTestConfig& cfg) {
  validateConfig(cfg);
  return createScaleTopology(cfg);
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
          // Matches legacy ScaleTestServer.cpp: kvManager is only useful when
          // SparkFaker is also active (the simulated neighbors drive the
          // per-neighbor KvStore servers).
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

  // Connect to the DUT. KvStoreThriftInjector::connect() and
  // DutMonitor::connect() both return false on failure; some lower-level paths
  // may also throw.
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

  // Patch the DUT into topology_. This is the only mutation of
  // topology_ in the Session lifecycle.
  const bool dutIsSpine =
      (*config_.topology()->dutRole() == thrift::DutRole::SPINE);
  if (!dutIsSpine) {
    DutPatcher::stripReplacedLeaf(topology_);
  }
  const auto dutNeighborNames = DutPatcher::buildDutNeighborNames(config_);
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

  if (kvManager_) {
    /*
     * The fake KvStore neighbor set must match the simulated topology: every
     * DUT neighbor (from buildDutNeighborNames, which uses the BBF leaf-N /
     * spine-N naming scheme) must be a real router in topology_. Generic
     * topologies (fabric/ring/grid) name nodes differently, so a topology-type
     * vs. neighbor-scheme mismatch would otherwise spin up phantom servers for
     * nodes that patchDutIntoTopology already WARN-skipped. Fail loudly here.
     * Thrown before the try below so it surfaces as TOPOLOGY_INVALID rather
     * than being remapped to INTERNAL.
     */
    if (auto missing =
            DutPatcher::missingNeighbors(topology_, dutNeighborNames);
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
      // LinkMonitor owns adj:<dut> on the real DUT; injecting it from the
      // test side would conflict with the DUT's own writes.
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

// Stubs for Step 3.
void
Session::downNode(const std::string&) {}
void
Session::upNode(const std::string&) {}
void
Session::downLink(const std::string&, const std::string&) {}
void
Session::upLink(const std::string&, const std::string&) {}
thrift::TestStatus
Session::getStatus() const {
  return {};
}
void
Session::onTimerTick() {}
void
Session::bumpFakeKeys() {}

} // namespace openr
