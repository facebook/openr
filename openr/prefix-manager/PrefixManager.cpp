/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManager.h"

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <folly/futures/Future.h>
#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>

namespace openr {

namespace {
// key for the persist config on disk
const std::string kConfigKey{"prefix-manager-config"};
// various error messages
const std::string kErrorNoPrefixToRemove{"No prefix to remove"};
const std::string kErrorNoPrefixesOfType{"No prefixes of type"};
const std::string kErrorUnknownCommand{"Unknown command"};
} // namespace

PrefixManager::PrefixManager(
    const std::string& nodeId,
    const PrefixManagerGlobalCmdUrl& globalCmdUrl,
    const PrefixManagerLocalCmdUrl& localCmdUrl,
    const PersistentStoreUrl& persistentStoreUrl,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const PrefixDbMarker& prefixDbMarker,
    bool enablePerfMeasurement,
    const MonitorSubmitUrl& monitorSubmitUrl,
    fbzmq::Context& zmqContext)
    : nodeId_{nodeId},
      globalCmdSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      localCmdSock_(
          zmqContext, folly::none, folly::none, fbzmq::NonblockingFlag{true}),
      configStoreClient_{persistentStoreUrl, zmqContext},
      prefixDbMarker_{prefixDbMarker},
      enablePerfMeasurement_{enablePerfMeasurement},
      kvStoreClient_{
          zmqContext, this, nodeId_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl} {
  CHECK(globalCmdSock_.bind(fbzmq::SocketUrl{globalCmdUrl}));
  CHECK(localCmdSock_.bind(fbzmq::SocketUrl{localCmdUrl}));

  addSocket(
      fbzmq::RawZmqSocketPtr{*globalCmdSock_},
      ZMQ_POLLIN,
      [this](int) noexcept {
        VLOG(2) << "PrefixManager: received request on global cmd socket";
        processRequest(globalCmdSock_);
      });

  addSocket(
      fbzmq::RawZmqSocketPtr{*localCmdSock_}, ZMQ_POLLIN, [this](int) noexcept {
        VLOG(2) << "PrefixManager: received request on local cmd socket";
        processRequest(localCmdSock_);
      });

  // pick up prefixes from disk
  auto maybePrefixDb =
      configStoreClient_.loadThriftObj<thrift::PrefixDatabase>(kConfigKey);
  if (maybePrefixDb) {
    LOG(INFO) << "Successfully loaded prefixes from disk";
    for (const auto& entry : maybePrefixDb.value().prefixEntries) {
      LOG(INFO) << "Loading Prefix: " << toString(entry.prefix);
      prefixMap_[entry.prefix] = entry;
    }
    persistPrefixDb();
  }

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

void
PrefixManager::processRequest(
    fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock) {
  fbzmq::Message requestIdMsg, delimMsg, thriftReqMsg;

  const auto ret = cmdSock.recvMultiple(requestIdMsg, delimMsg, thriftReqMsg);

  if (ret.hasError()) {
    LOG(ERROR) << "processRequest: Error receiving command: " << ret.error();
    return;
  }

  if (not delimMsg.empty()) {
    LOG(ERROR) << "processRequest: Non-empty delimiter";
    return;
  }

  const auto maybeThriftReq =
      thriftReqMsg.readThriftObj<thrift::PrefixManagerRequest>(serializer_);
  if (maybeThriftReq.hasError()) {
    LOG(ERROR) << "processRequest: failed reading thrift::PrefixRequest "
               << maybeThriftReq.error();
    return;
  }

  const auto& thriftReq = maybeThriftReq.value();
  thrift::PrefixManagerResponse response;
  switch (thriftReq.cmd) {
  case thrift::PrefixManagerCommand::ADD_PREFIXES: {
    tData_.addStatValue("prefix_manager.add_prefixes", 1, fbzmq::COUNT);
    addOrUpdatePrefixes(thriftReq.prefixes);
    persistPrefixDb();
    response.success = true;
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
    syncPrefixesByType(thriftReq.type, thriftReq.prefixes);
    persistPrefixDb();
    response.success = true;
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

  auto sndRet = cmdSock.sendMultiple(
      requestIdMsg,
      delimMsg,
      fbzmq::Message::fromThriftObj(response, serializer_).value());
  if (sndRet.hasError()) {
    LOG(ERROR) << "Error sending response. " << sndRet.error();
  }
}

void
PrefixManager::submitCounters() {
  VLOG(2) << "Submitting counters ... ";

  // Extract/build counters from thread-data
  auto counters = tData_.getCounters();

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
void
PrefixManager::addOrUpdatePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  for (const auto& prefix : prefixes) {
    prefixMap_[prefix.prefix] = prefix;
  }
}

bool
PrefixManager::removePrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  bool fail{false};

  for (const auto& prefix : prefixes) {
    fail = prefixMap_.erase(prefix.prefix) > 0 or fail;
  }
  return fail;
}

void
PrefixManager::syncPrefixesByType(
    thrift::PrefixType type, const std::vector<thrift::PrefixEntry>& prefixes) {
  // remove all prefixes of this type
  removePrefixesByType(type);
  addOrUpdatePrefixes(prefixes);
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
