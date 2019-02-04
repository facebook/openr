/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <folly/futures/Future.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

namespace {
// key for the persist config on disk
const std::string kConfigKey{"prefix-manager-config"};
// various error messages
const std::string kErrorNoChanges{"No changes in prefixes to be advertised"};
const std::string kErrorNoPrefixToRemove{"No prefix to remove"};
const std::string kErrorNoPrefixesOfType{"No prefixes of type"};
const std::string kErrorUnknownCommand{"Unknown command"};
} // namespace

PrefixManager::PrefixManager(
    const std::string& nodeId,
    const folly::Optional<std::string>& globalCmdUrl,
    const PersistentStoreUrl& persistentStoreUrl,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const PrefixDbMarker& prefixDbMarker,
    bool enablePerfMeasurement,
    const std::chrono::seconds prefixHoldTime,
    fbzmq::Context& zmqContext)
    : OpenrEventLoop(
          nodeId,
          thrift::OpenrModuleType::PREFIX_MANAGER,
          zmqContext,
          globalCmdUrl),
      nodeId_(nodeId),
      configStoreClient_{persistentStoreUrl, zmqContext},
      prefixDbMarker_{prefixDbMarker},
      enablePerfMeasurement_{enablePerfMeasurement},
      prefixHoldUntilTimePoint_(
          std::chrono::steady_clock::now() + prefixHoldTime),
      kvStoreClient_{
          zmqContext, this, nodeId_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl} {
  // pick up prefixes from disk
  auto maybePrefixDb =
      configStoreClient_.loadThriftObj<thrift::PrefixDatabase>(kConfigKey);
  if (maybePrefixDb.hasValue()) {
    LOG(INFO) << "Successfully loaded " << maybePrefixDb->prefixEntries.size()
              << " prefixes from disk";
    for (const auto& entry : maybePrefixDb.value().prefixEntries) {
      LOG(INFO) << "  > " << toString(entry.prefix);
      prefixMap_[entry.prefix] = entry;
    }
    // Prefixes will be advertised after prefixHoldUntilTimePoint_
  }

  // Create throttled version of persistPrefixDbThrottled_
  scheduleTimeoutAt(prefixHoldUntilTimePoint_, [this]() { persistPrefixDb(); });

  zmqMonitorClient_ =
      std::make_unique<fbzmq::ZmqMonitorClient>(zmqContext, monitorSubmitUrl);

  // Schedule periodic timer for submission to monitor
  const bool isPeriodic = true;
  monitorTimer_ =
      fbzmq::ZmqTimeout::make(this, [this]() noexcept { submitCounters(); });
  monitorTimer_->scheduleTimeout(Constants::kMonitorSubmitInterval, isPeriodic);
}

void
PrefixManager::persistPrefixDb() {
  if (std::chrono::steady_clock::now() < prefixHoldUntilTimePoint_) {
    // Too early for advertising my own prefixes. Let timeout advertise it
    // and skip here.
    return;
  }

  // our prefixDb has changed, save the newest to disk
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName = nodeId_;
  for (const auto& kv : prefixMap_) {
    prefixDb.prefixEntries.emplace_back(kv.second);
  }

  // Add perf information if enabled
  if (enablePerfMeasurement_) {
    thrift::PerfEvents perfEvents;
    addPerfEvent(perfEvents, nodeId_, "PREFIX_DB_UPDATED");
    prefixDb.perfEvents = perfEvents;
  } else {
    DCHECK(!prefixDb.perfEvents.hasValue());
  }

  auto ret = configStoreClient_.storeThriftObj(kConfigKey, prefixDb);
  if (ret.hasError()) {
    LOG(ERROR) << "Error saving prefixDb to file";
  }

  const auto prefixDbVal =
      fbzmq::util::writeThriftObjStr(prefixDb, serializer_);
  const auto prefixDbKey = folly::sformat(
      "{}{}", static_cast<std::string>(prefixDbMarker_), nodeId_);

  LOG(INFO) << "writing my prefix to KvStore " << prefixDbKey;
  kvStoreClient_.persistKey(prefixDbKey, prefixDbVal, Constants::kKvStoreDbTtl);
}

folly::Expected<fbzmq::Message, fbzmq::Error>
PrefixManager::processRequestMsg(fbzmq::Message&& request) {
  const auto maybeThriftReq =
      request.readThriftObj<thrift::PrefixManagerRequest>(serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::PrefixRequest "
               << maybeThriftReq.error();
    return folly::makeUnexpected(fbzmq::Error());
  }

  const auto& thriftReq = maybeThriftReq.value();
  thrift::PrefixManagerResponse response;
  switch (thriftReq.cmd) {
  case thrift::PrefixManagerCommand::ADD_PREFIXES: {
    tData_.addStatValue("prefix_manager.add_prefixes", 1, fbzmq::COUNT);
    if (addOrUpdatePrefixes(thriftReq.prefixes)) {
      persistPrefixDb();
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoChanges;
    }

    break;
  }
  case thrift::PrefixManagerCommand::WITHDRAW_PREFIXES: {
    if (removePrefixes(thriftReq.prefixes)) {
      persistPrefixDb();
      response.success = true;
      tData_.addStatValue("prefix_manager.withdraw_prefixes", 1, fbzmq::COUNT);
    } else {
      response.success = false;
      response.message = kErrorNoPrefixToRemove;
    }
    break;
  }
  case thrift::PrefixManagerCommand::WITHDRAW_PREFIXES_BY_TYPE: {
    if (removePrefixesByType(thriftReq.type)) {
      persistPrefixDb();
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoPrefixesOfType;
    }
    break;
  }
  case thrift::PrefixManagerCommand::SYNC_PREFIXES_BY_TYPE: {
    if (syncPrefixesByType(thriftReq.type, thriftReq.prefixes)) {
      persistPrefixDb();
      response.success = true;
    } else {
      response.success = false;
      response.message = kErrorNoChanges;
    }
    break;
  }
  case thrift::PrefixManagerCommand::GET_ALL_PREFIXES: {
    for (const auto& kv : prefixMap_) {
      response.prefixes.emplace_back(kv.second);
    }
    response.success = true;
    break;
  }
  case thrift::PrefixManagerCommand::GET_PREFIXES_BY_TYPE: {
    for (const auto& kv : prefixMap_) {
      if (kv.second.type == thriftReq.type) {
        response.prefixes.emplace_back(kv.second);
      }
    }
    response.success = true;
    break;
  }
  default: {
    LOG(ERROR) << "Unknown command received";
    response.success = false;
    response.message = kErrorUnknownCommand;
    break;
  }
  }

  return fbzmq::Message::fromThriftObj(response, serializer_);
}

void
PrefixManager::submitCounters() {
  VLOG(2) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();
  counters["prefix_manager.zmq_event_queue_size"] = getEventQueueSize();

  zmqMonitorClient_->setCounters(prepareSubmitCounters(std::move(counters)));
}

int64_t
PrefixManager::getCounter(const std::string& key) {
  std::unordered_map<std::string, int64_t> counters;

  folly::Promise<std::unordered_map<std::string, int64_t>> promise;
  auto future = promise.getFuture();
  runImmediatelyOrInEventLoop([this, promise = std::move(promise)]() mutable {
    promise.setValue(tData_.getCounters());
  });
  counters = std::move(future).get();

  if (counters.find(key) != counters.end()) {
    return counters[key];
  }
  return 0;
}

int64_t
PrefixManager::getPrefixAddCounter() {
  return getCounter("prefix_manager.add_prefixes.count.0");
}

int64_t
PrefixManager::getPrefixWithdrawCounter() {
  return getCounter("prefix_manager.withdraw_prefixes.count.0");
}

// helpers for modifying our Prefix Db
bool
PrefixManager::addOrUpdatePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  bool updated{false};
  for (const auto& prefix : prefixes) {
    LOG(INFO) << "Advertising prefix " << toString(prefix.prefix)
              << ", client: "
              << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                     prefix.type);
    auto it = prefixMap_.find(prefix.prefix);
    if (it == prefixMap_.end()) {
      // Add missing prefix
      prefixMap_.emplace(prefix.prefix, prefix);
      updated = true;
    } else if (it->second != prefix) {
      it->second = prefix;
      updated = true;
    }
  }

  return updated;
}

bool
PrefixManager::removePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  bool fail{false};

  for (const auto& prefix : prefixes) {
    LOG(INFO) << "Withdrawing prefix " << toString(prefix.prefix)
              << ", client: "
              << apache::thrift::TEnumTraits<thrift::PrefixType>::findName(
                     prefix.type);
    fail = prefixMap_.erase(prefix.prefix) > 0 or fail;
  }
  return fail;
}

bool
PrefixManager::syncPrefixesByType(
    thrift::PrefixType type, const std::vector<thrift::PrefixEntry>& prefixes) {
  bool updated{false};

  // Remove old prefixes
  std::unordered_set<thrift::IpPrefix> newPrefixes;
  for (auto const& prefix : prefixes) {
    newPrefixes.emplace(prefix.prefix);
  }
  for (auto it = prefixMap_.begin(); it != prefixMap_.end();) {
    if (it->second.type == type and newPrefixes.count(it->first) == 0) {
      it = prefixMap_.erase(it);
      updated = true;
    } else {
      ++it;
    }
  }

  // Add/update new prefixes
  updated |= addOrUpdatePrefixes(prefixes);

  return updated;
}

bool
PrefixManager::removePrefixesByType(thrift::PrefixType type) {
  bool changed = false;
  for (auto iter = prefixMap_.begin(); iter != prefixMap_.end();) {
    if (iter->second.type == type) {
      changed = true;
      iter = prefixMap_.erase(iter);
    } else {
      ++iter;
    }
  }
  return changed;
}

} // namespace openr
