/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/Session.h>

#include <openr/tests/scale/DutPatcher.h>

#include <fmt/format.h>

#if __has_include("openr/tests/scale/facebook/BbfTopologyGenerator.h")
#include "openr/tests/scale/facebook/BbfTopologyGenerator.h"
#define OPENR_HAS_BBF_TOPOLOGY 1
#endif

namespace openr {

namespace {

constexpr int kFakeKvStoreIoThreads = 32;

// Throws thrift::SetupError if any required field of ScaleTestConfig is unset.
// All scale knobs and side-effecting bools are optional in the IDL so the
// server can distinguish "unset" from zero/false and reject both.
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
  const auto& t = *cfg.topology();
  // topology.type has an IDL default; validate it's one we recognise so we
  // fail at the validation boundary instead of falling through buildTopology.
  if (*t.type() != "bbf-simple" && *t.type() != "bbf-full") {
    fail(
        fmt::format(
            "TopologyConfig.type must be 'bbf-simple' or 'bbf-full' (got '{}')",
            *t.type()));
  }
  if (!t.numSpines().has_value()) {
    fail("TopologyConfig.numSpines must be set");
  }
  if (!t.numLeaves().has_value()) {
    fail("TopologyConfig.numLeaves must be set");
  }
  if (!t.numSuperSpines().has_value()) {
    fail("TopologyConfig.numSuperSpines must be set");
  }
  if (!t.numPods().has_value()) {
    fail("TopologyConfig.numPods must be set");
  }
  if (!t.numSites().has_value()) {
    fail("TopologyConfig.numSites must be set");
  }
  if (!t.numPrefixesPerNode().has_value()) {
    fail("TopologyConfig.numPrefixesPerNode must be set");
  }
  if (!t.dutRole().has_value()) {
    fail("TopologyConfig.dutRole must be set");
  }
  if (!t.ecmpWidth().has_value()) {
    fail("TopologyConfig.ecmpWidth must be set");
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

Topology
buildTopology(const thrift::ScaleTestConfig& cfg) {
  validateConfig(cfg);
  const auto& t = *cfg.topology();
#ifdef OPENR_HAS_BBF_TOPOLOGY
  if (*t.type() == "bbf-simple") {
    return BbfTopologyGenerator::createBbfSimple(
        *t.numSpines(),
        *t.numLeaves(),
        *t.numSuperSpines(),
        *t.ecmpWidth(),
        *t.numPrefixesPerNode(),
        *t.numSites());
  }
  return BbfTopologyGenerator::createBbf(
      *t.numPods(),
      *t.numSpines(),
      kDefaultBbfSpinesPerPlane,
      kDefaultBbfLeavesPerPod,
      *t.ecmpWidth(),
      *t.numPrefixesPerNode());
#else
  thrift::SetupError se;
  se.reason() = thrift::SetupErrorReason::TOPOLOGY_INVALID;
  se.message() = fmt::format(
      "Topology type '{}' is not available in this build", *t.type());
  throw se;
#endif
}

} // namespace

Session::Session(const thrift::ScaleTestConfig& cfg, int basePortOverride)
    : config_(cfg),
      topology_(buildTopology(cfg)),
      startedAt_(std::chrono::steady_clock::now()),
      injector_(
          std::make_unique<KvStoreThriftInjector>(
              *cfg.dut()->host(), static_cast<uint16_t>(*cfg.dut()->port()))),
      dutMonitor_(
          std::make_shared<DutMonitor>(
              *cfg.dut()->host(), static_cast<uint16_t>(*cfg.dut()->port()))),
      kvManager_(
          cfg.injection()->enableFakeKvStore().value_or(false)
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
