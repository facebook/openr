/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/ctrl-server/OpenrCtrlHandler.h>

#include <re2/re2.h>

#include <folly/ExceptionString.h>
#include <folly/io/async/SSLContext.h>
#include <folly/io/async/ssl/OpenSSLUtils.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/monitor/LogSample.h>
#include <openr/prefix-manager/PrefixManager.h>

namespace fb303 = facebook::fb303;

namespace openr {

OpenrCtrlHandler::OpenrCtrlHandler(
    const std::string& nodeName,
    const std::unordered_set<std::string>& acceptablePeerCommonNames,
    OpenrEventBase* ctrlEvb,
    Decision* decision,
    Fib* fib,
    KvStore* kvStore,
    LinkMonitor* linkMonitor,
    Monitor* monitor,
    PersistentStore* configStore,
    PrefixManager* prefixManager,
    Spark* spark,
    std::shared_ptr<const Config> config)
    : fb303::BaseService("openr"),
      nodeName_(nodeName),
      acceptablePeerCommonNames_(acceptablePeerCommonNames),
      decision_(decision),
      fib_(fib),
      kvStore_(kvStore),
      linkMonitor_(linkMonitor),
      monitor_(monitor),
      configStore_(configStore),
      prefixManager_(prefixManager),
      spark_(spark),
      config_(config) {
  // Add fiber task to receive publication from KvStore
  if (kvStore_) {
    auto taskFutureKvStore = ctrlEvb->addFiberTaskFuture([
      q = std::move(kvStore_->getKvStoreUpdatesReader()),
      this
    ]() mutable noexcept {
      LOG(INFO) << "Starting KvStore updates processing fiber";
      while (true) {
        auto maybePublication = q.get(); // perform read
        VLOG(2) << "Received publication from KvStore";
        if (maybePublication.hasError()) {
          LOG(INFO) << "Terminating KvStore publications processing fiber";
          break;
        }

        SYNCHRONIZED(kvStorePublishers_) {
          for (auto& kv : kvStorePublishers_) {
            kv.second->publish(maybePublication.value());
          }
        }

        bool isAdjChanged = false;
        // check if any of KeyVal has 'adj' update
        for (auto& kv : *maybePublication.value().keyVals_ref()) {
          auto& key = kv.first;
          auto& val = kv.second;
          // check if we have any value update.
          // Ttl refreshing won't update any value.
          if (!val.value_ref().has_value()) {
            continue;
          }

          // "adj:*" key has changed. Update local collection
          if (key.find(Constants::kAdjDbMarker.toString()) == 0) {
            VLOG(3) << "Adj key: " << key << " change received";
            isAdjChanged = true;
            break;
          }
        }

        if (isAdjChanged) {
          // thrift::Publication contains "adj:*" key change.
          // Clean ALL pending promises
          longPollReqs_.withWLock([&](auto& longPollReqs) {
            for (auto& kv : longPollReqs) {
              auto& p = kv.second.first;
              p.setValue(true);
            }
            longPollReqs.clear();
          });
        } else {
          longPollReqs_.withWLock([&](auto& longPollReqs) {
            auto now = getUnixTimeStampMs();
            std::vector<int64_t> reqsToClean;
            for (auto& kv : longPollReqs) {
              auto& clientId = kv.first;
              auto& req = kv.second;

              auto& p = req.first;
              auto& timeStamp = req.second;
              if (now - timeStamp >= Constants::kLongPollReqHoldTime.count()) {
                LOG(INFO) << "Elapsed time: " << now - timeStamp
                          << " is over hold limit: "
                          << Constants::kLongPollReqHoldTime.count();
                reqsToClean.emplace_back(clientId);
                p.setValue(false);
              }
            }

            // cleanup expired requests since no ADJ change observed
            for (auto& clientId : reqsToClean) {
              longPollReqs.erase(clientId);
            }
          });
        }
      }
    });

    workers_.push_back(std::move(taskFutureKvStore));
  }

  // Add fiber task to receive fib streaming
  if (fib_) {
    auto taskFutureFib = ctrlEvb->addFiberTaskFuture(
        [q = std::move(fib_->getFibUpdatesReader()), this]() mutable noexcept {
          LOG(INFO) << "Starting Fib updates processing fiber";
          while (true) {
            auto maybeUpdate = q.get(); // perform read
            VLOG(2) << "Received RouteDatabaseDelta from Fib";
            if (maybeUpdate.hasError()) {
              LOG(INFO)
                  << "Terminating Fib RouteDatabaseDelta processing fiber";
              break;
            }

            // Publish the update to all active streams
            fibPublishers_.withWLock([&maybeUpdate](auto& fibPublishers) {
              for (auto& fibPublisher : fibPublishers) {
                fibPublisher.second.next(maybeUpdate.value());
              }
            });
          }
        });

    workers_.push_back(std::move(taskFutureFib));
  }
}

OpenrCtrlHandler::~OpenrCtrlHandler() {
  closeKvStorePublishers();
  closeFibPublishers();

  LOG(INFO) << "Cleanup all pending request(s).";
  longPollReqs_.withWLock([&](auto& longPollReqs) { longPollReqs.clear(); });

  LOG(INFO)
      << "Waiting for termination of kvStoreUpdatesQueue, FibUpdatesQueue";
  folly::collectAll(workers_.begin(), workers_.end()).get();
  LOG(INFO) << "Collected all workers";
}

// NOTE: We're intentionally creating list of publishers to and then invoke
// `complete()` on them.
// Reason => `complete()` returns only when callback `onComplete` associated
// with publisher returns. Since we acquire lock within `onComplete` callback,
// we will run into the deadlock if `complete()` is invoked within
// SYNCHRONIZED block
void
OpenrCtrlHandler::closeKvStorePublishers() {
  std::vector<std::unique_ptr<KvStorePublisher>> publishers;
  SYNCHRONIZED(kvStorePublishers_) {
    for (auto& kv : kvStorePublishers_) {
      publishers.emplace_back(std::move(kv.second));
    }
  }
  LOG(INFO) << "Terminating " << publishers.size()
            << " active KvStore snoop stream(s).";
  for (auto& publisher : publishers) {
    // We have to send an exception as part of the completion, otherwise
    // thrift doesn't seem to notify the peer of the shutdown
    publisher->complete(folly::make_exception_wrapper<std::runtime_error>(
        "publisher terminated"));
  }
}

// Refer to note on top of closeKvStorePublishers
void
OpenrCtrlHandler::closeFibPublishers() {
  std::vector<apache::thrift::ServerStreamPublisher<thrift::RouteDatabaseDelta>>
      fibPublishers_close;
  fibPublishers_.withWLock([&fibPublishers_close](auto& fibPublishers) {
    for (auto& kv : fibPublishers) {
      fibPublishers_close.emplace_back(std::move(kv.second));
    }
  });
  LOG(INFO) << "Terminating " << fibPublishers_close.size()
            << " active Fib snoop stream(s).";
  for (auto& fibPublisher : fibPublishers_close) {
    std::move(fibPublisher).complete();
  }
}

void
OpenrCtrlHandler::authorizeConnection() {
  auto connContext = getConnectionContext()->getConnectionContext();
  auto peerCommonName = connContext->getPeerCommonName();
  auto peerAddr = connContext->getPeerAddress();

  // We legitely accepts all connections (secure/non-secure) from localhost
  if (peerAddr->isLoopbackAddress()) {
    return;
  }

  if (peerCommonName.empty() || acceptablePeerCommonNames_.empty()) {
    // for now, we will allow non-secure connections, but lets log the event so
    // we know how often this is happening.
    LogSample sample{};

    sample.addString(
        "event",
        peerCommonName.empty() ? "UNENCRYPTED_CTRL_CONNECTION"
                               : "UNRESTRICTED_AUTHORIZATION");
    sample.addString("node_name", nodeName_);
    sample.addString(
        "peer_address", connContext->getPeerAddress()->getAddressStr());
    sample.addString("peer_common_name", peerCommonName);

    LOG(INFO) << "Authorizing request with issues: " << sample.toJson();
    return;
  }

  if (!acceptablePeerCommonNames_.count(peerCommonName)) {
    throw thrift::OpenrError(
        folly::sformat("Peer name {} is unacceptable", peerCommonName));
  }
}

fb303::cpp2::fb303_status
OpenrCtrlHandler::getStatus() {
  return fb303::cpp2::fb303_status::ALIVE;
}

void
OpenrCtrlHandler::getEventLogs(std::vector<::std::string>& _return) {
  auto recentEventLogs = monitor_->getRecentEventLogs();
  for (auto const& log : recentEventLogs) {
    _return.emplace_back(log);
  }
}

void
OpenrCtrlHandler::getCounters(std::map<std::string, int64_t>& _return) {
  BaseService::getCounters(_return);
}

void
OpenrCtrlHandler::getRegexCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::string> regex) {
  // Compile regex
  re2::RE2 compiledRegex(*regex);
  if (not compiledRegex.ok()) {
    return;
  }

  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& kv : counters) {
    if (RE2::PartialMatch(kv.first, compiledRegex)) {
      _return.emplace(kv);
    }
  }
}

void
OpenrCtrlHandler::getSelectedCounters(
    std::map<std::string, int64_t>& _return,
    std::unique_ptr<std::vector<std::string>> keys) {
  // Get all counters
  std::map<std::string, int64_t> counters;
  getCounters(counters);

  // Filter counters
  for (auto const& key : *keys) {
    auto it = counters.find(key);
    if (it != counters.end()) {
      _return.emplace(*it);
    }
  }
}

int64_t
OpenrCtrlHandler::getCounter(std::unique_ptr<std::string> key) {
  return BaseService::getCounter(std::move(key));
}

void
OpenrCtrlHandler::getMyNodeName(std::string& _return) {
  _return = std::string(nodeName_);
}

void
OpenrCtrlHandler::getOpenrVersion(thrift::OpenrVersions& _openrVersion) {
  _openrVersion.version_ref() = Constants::kOpenrVersion;
  _openrVersion.lowestSupportedVersion_ref() =
      Constants::kOpenrSupportedVersion;
}

void
OpenrCtrlHandler::getBuildInfo(thrift::BuildInfo& _buildInfo) {
  _buildInfo = getBuildInfoThrift();
}

// validate config
void
OpenrCtrlHandler::dryrunConfig(
    std::string& _return, std::unique_ptr<std::string> file) {
  if (not file) {
    throw thrift::OpenrError("Dereference nullptr for config file");
  }

  // check if the config file exists and readable
  const auto& fileName = *file;
  if (not fileExists(fileName)) {
    throw thrift::OpenrError(
        folly::sformat("Config file doesn't exist: {}", fileName));
  }

  try {
    // Create policy manager using new config file
    auto config = Config(fileName);
    _return = config.getRunningConfig();
  } catch (const std::exception& ex) {
    throw thrift::OpenrError(ex.what());
  }
}

void
OpenrCtrlHandler::getRunningConfig(std::string& _return) {
  _return = config_->getRunningConfig();
}

void
OpenrCtrlHandler::getRunningConfigThrift(thrift::OpenrConfig& _config) {
  _config = config_->getConfig();
}

//
// PrefixManager APIs
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_advertisePrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->advertisePrefixes(std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixes(
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->withdrawPrefixes(std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_withdrawPrefixesByType(
    thrift::PrefixType prefixType) {
  CHECK(prefixManager_);
  return prefixManager_->withdrawPrefixesByType(prefixType)
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_syncPrefixesByType(
    thrift::PrefixType prefixType,
    std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) {
  CHECK(prefixManager_);
  return prefixManager_->syncPrefixesByType(prefixType, std::move(*prefixes))
      .defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixes() {
  CHECK(prefixManager_);
  return prefixManager_->getPrefixes();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
OpenrCtrlHandler::semifuture_getPrefixesByType(thrift::PrefixType prefixType) {
  CHECK(prefixManager_);
  return prefixManager_->getPrefixesByType(prefixType);
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
OpenrCtrlHandler::semifuture_getAdvertisedRoutes() {
  auto filter = std::make_unique<thrift::AdvertisedRouteFilter>();
  return semifuture_getAdvertisedRoutesFiltered(std::move(filter));
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
OpenrCtrlHandler::semifuture_getAdvertisedRoutesFiltered(
    std::unique_ptr<thrift::AdvertisedRouteFilter> filter) {
  CHECK(prefixManager_);
  return prefixManager_->getAdvertisedRoutesFiltered(std::move(*filter));
}

//
// Fib APIs
//

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDb() {
  CHECK(fib_);
  return fib_->getRouteDb();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
OpenrCtrlHandler::semifuture_getUnicastRoutesFiltered(
    std::unique_ptr<std::vector<std::string>> prefixes) {
  CHECK(fib_);
  return fib_->getUnicastRoutes(std::move(*prefixes));
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
OpenrCtrlHandler::semifuture_getUnicastRoutes() {
  folly::Promise<std::unique_ptr<std::vector<thrift::UnicastRoute>>> p;
  CHECK(fib_);
  return fib_->getUnicastRoutes({});
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
OpenrCtrlHandler::semifuture_getMplsRoutes() {
  CHECK(fib_);
  return fib_->getMplsRoutes({});
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
OpenrCtrlHandler::semifuture_getMplsRoutesFiltered(
    std::unique_ptr<std::vector<int32_t>> labels) {
  CHECK(fib_);
  return fib_->getMplsRoutes(std::move(*labels));
}

//
// Spark APIs
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_floodRestartingMsg() {
  CHECK(spark_);
  return spark_->floodRestartingMsg();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::SparkNeighbor>>>
OpenrCtrlHandler::semifuture_getNeighbors() {
  CHECK(spark_);
  return spark_->getNeighbors();
}

//
// Performance stats APIs
//
folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
OpenrCtrlHandler::semifuture_getPerfDb() {
  CHECK(fib_);
  return fib_->getPerfDb();
}

//
// Decision APIs
//

folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
OpenrCtrlHandler::semifuture_getReceivedRoutes() {
  auto filter = std::make_unique<thrift::ReceivedRouteFilter>();
  return semifuture_getReceivedRoutesFiltered(std::move(filter));
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
OpenrCtrlHandler::semifuture_getReceivedRoutesFiltered(
    std::unique_ptr<thrift::ReceivedRouteFilter> filter) {
  CHECK(decision_);
  return decision_->getReceivedRoutesFiltered(std::move(*filter));
}

folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
OpenrCtrlHandler::semifuture_getRouteDbComputed(
    std::unique_ptr<std::string> nodeName) {
  CHECK(decision_);
  return decision_->getDecisionRouteDb(*nodeName);
}

folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
OpenrCtrlHandler::semifuture_getDecisionAdjacencyDbs() {
  CHECK(decision_);
  return decision_->getDecisionAdjacencyDbs();
}

folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
OpenrCtrlHandler::semifuture_getAllDecisionAdjacencyDbs() {
  CHECK(decision_);
  return decision_->getAllDecisionAdjacencyDbs();
}

folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
OpenrCtrlHandler::semifuture_getDecisionPrefixDbs() {
  CHECK(decision_);
  return decision_->getDecisionPrefixDbs();
}

//
// KvStore APIs
//
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyVals(
    std::unique_ptr<std::vector<std::string>> filterKeys) {
  thrift::KeyGetParams params;
  *params.keys_ref() = std::move(*filterKeys);

  CHECK(kvStore_);
  return kvStore_->getKvStoreKeyVals(std::move(params));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  thrift::KeyGetParams params;
  *params.keys_ref() = std::move(*filterKeys);

  CHECK(kvStore_);
  return kvStore_->getKvStoreKeyVals(std::move(params), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreKeys(std::move(*filter));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreKeyValsFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreKeys(std::move(*filter), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreHashFiltered(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreHashes(std::move(*filter));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
OpenrCtrlHandler::semifuture_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->dumpKvStoreHashes(std::move(*filter), std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->setKvStoreKeyVals(std::move(*setParams), std::move(*area));
}

folly::SemiFuture<bool>
OpenrCtrlHandler::semifuture_longPollKvStoreAdj(
    std::unique_ptr<thrift::KeyVals> snapshot) {
  folly::Promise<bool> p;
  auto sf = p.getSemiFuture();

  auto timeStamp = getUnixTimeStampMs();
  auto requestId = pendingRequestId_++;

  thrift::KeyDumpParams params;

  // build thrift::KeyVals with "adj:" key ONLY
  // to ensure KvStore ONLY compare "adj:" key
  thrift::KeyVals adjKeyVals;
  for (auto& kv : *snapshot) {
    if (kv.first.find(Constants::kAdjDbMarker.toString()) == 0) {
      adjKeyVals.emplace(kv.first, kv.second);
    }
  }

  // Only care about "adj:" key
  *params.prefix_ref() = Constants::kAdjDbMarker;
  params.keys_ref() = {Constants::kAdjDbMarker.toString()};
  // Only dump difference between KvStore and client snapshot
  params.keyValHashes_ref() = std::move(adjKeyVals);

  // Explicitly do SYNC call to KvStore
  std::unique_ptr<thrift::Publication> thriftPub{nullptr};
  try {
    thriftPub = semifuture_getKvStoreKeyValsFiltered(
                    std::make_unique<thrift::KeyDumpParams>(params))
                    .get();
  } catch (std::exception const& ex) {
    p.setException(thrift::OpenrError(ex.what()));
    return sf;
  }

  if (thriftPub->keyVals_ref()->size() > 0) {
    VLOG(3) << "AdjKey has been added/modified. Notify immediately";
    p.setValue(true);
  } else if (
      thriftPub->tobeUpdatedKeys_ref().has_value() &&
      thriftPub->tobeUpdatedKeys_ref().value().size() > 0) {
    VLOG(3) << "AdjKey has been deleted/expired. Notify immediately";
    p.setValue(true);
  } else {
    // Client provided data is consistent with KvStore.
    // Store req for future processing when there is publication
    // from KvStore.
    VLOG(3) << "No adj change detected. Store req as pending request";
    longPollReqs_.withWLock([&](auto& longPollReq) {
      longPollReq.emplace(requestId, std::make_pair(std::move(p), timeStamp));
    });
  }
  return sf;
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_processKvStoreDualMessage(
    std::unique_ptr<thrift::DualMessages> messages,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->processKvStoreDualMessage(
      std::move(*messages), std::move(*area));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_updateFloodTopologyChild(
    std::unique_ptr<thrift::FloodTopoSetParams> params,
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->updateFloodTopologyChild(
      std::move(*params), std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
OpenrCtrlHandler::semifuture_getSpanningTreeInfos(
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->getSpanningTreeInfos(std::move(*area));
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeers() {
  CHECK(kvStore_);
  return kvStore_->getKvStorePeers();
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
OpenrCtrlHandler::semifuture_getKvStorePeersArea(
    std::unique_ptr<std::string> area) {
  CHECK(kvStore_);
  return kvStore_->getKvStorePeers(std::move(*area));
}

apache::thrift::ServerStream<thrift::Publication>
OpenrCtrlHandler::subscribeKvStoreFilter(
    std::unique_ptr<thrift::KeyDumpParams> filter) {
  // Get new client-ID (monotonically increasing)
  auto clientToken = publisherToken_++;

  auto streamAndPublisher =
      apache::thrift::ServerStream<thrift::Publication>::createPublisher(
          [this, clientToken]() {
            SYNCHRONIZED(kvStorePublishers_) {
              if (kvStorePublishers_.erase(clientToken)) {
                LOG(INFO) << "KvStore snoop stream-" << clientToken
                          << " ended.";
              } else {
                LOG(ERROR) << "Can't remove unknown KvStore snoop stream-"
                           << clientToken;
              }
              fb303::fbData->setCounter(
                  "subscribers.kvstore", kvStorePublishers_.size());
            }
          });

  SYNCHRONIZED(kvStorePublishers_) {
    assert(kvStorePublishers_.count(clientToken) == 0);
    LOG(INFO) << "KvStore snoop stream-" << clientToken << " started.";
    auto kvStorePublisher = std::make_unique<KvStorePublisher>(
        std::move(*filter), std::move(streamAndPublisher.second));
    kvStorePublishers_.emplace(clientToken, std::move(kvStorePublisher));
    fb303::fbData->setCounter("subscribers.kvstore", kvStorePublishers_.size());
  }
  return std::move(streamAndPublisher.first);
}

folly::SemiFuture<apache::thrift::ResponseAndServerStream<
    thrift::Publication,
    thrift::Publication>>
OpenrCtrlHandler::semifuture_subscribeAndGetKvStore() {
  return semifuture_subscribeAndGetKvStoreFiltered(
      std::make_unique<thrift::KeyDumpParams>());
}

folly::SemiFuture<apache::thrift::ResponseAndServerStream<
    thrift::Publication,
    thrift::Publication>>
OpenrCtrlHandler::semifuture_subscribeAndGetKvStoreFiltered(
    std::unique_ptr<thrift::KeyDumpParams> dumpParams) {
  auto subscriptionParams =
      std::make_unique<thrift::KeyDumpParams>(*dumpParams);
  return semifuture_getKvStoreKeyValsFiltered(std::move(dumpParams))
      .defer(
          [stream = subscribeKvStoreFilter(std::move(subscriptionParams))](
              folly::Try<std::unique_ptr<thrift::Publication>>&& pub) mutable {
            pub.throwIfFailed();
            return apache::thrift::ResponseAndServerStream<
                thrift::Publication,
                thrift::Publication>{std::move(*pub.value()),
                                     std::move(stream)};
          });
}

apache::thrift::ServerStream<thrift::RouteDatabaseDelta>
OpenrCtrlHandler::subscribeFib() {
  // Get new client-ID (monotonically increasing)
  auto clientToken = publisherToken_++;

  auto streamAndPublisher =
      apache::thrift::ServerStream<thrift::RouteDatabaseDelta>::createPublisher(
          [this, clientToken]() {
            fibPublishers_.withWLock([&clientToken](auto& fibPublishers) {
              if (fibPublishers.erase(clientToken)) {
                LOG(INFO) << "Fib snoop stream-" << clientToken << " ended.";
              } else {
                LOG(ERROR) << "Can't remove unknown Fib snoop stream-"
                           << clientToken;
              }
              fb303::fbData->setCounter(
                  "subscribers.fib", fibPublishers.size());
            });
          });

  fibPublishers_.withWLock([&clientToken,
                            &streamAndPublisher](auto& fibPublishers) {
    assert(fibPublishers.count(clientToken) == 0);
    LOG(INFO) << "Fib snoop stream-" << clientToken << " started.";
    fibPublishers.emplace(clientToken, std::move(streamAndPublisher.second));
    fb303::fbData->setCounter("subscribers.fib", fibPublishers.size());
  });
  return std::move(streamAndPublisher.first);
}

folly::SemiFuture<apache::thrift::ResponseAndServerStream<
    thrift::RouteDatabase,
    thrift::RouteDatabaseDelta>>
OpenrCtrlHandler::semifuture_subscribeAndGetFib() {
  auto stream = subscribeFib();
  return semifuture_getRouteDb().defer(
      [stream = std::move(stream)](
          folly::Try<std::unique_ptr<thrift::RouteDatabase>>&& db) mutable {
        db.throwIfFailed();
        return apache::thrift::ResponseAndServerStream<
            thrift::RouteDatabase,
            thrift::RouteDatabaseDelta>{std::move(*db.value()),
                                        std::move(stream)};
      });
}

//
// LinkMonitor APIs
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setNodeOverload() {
  CHECK(linkMonitor_);
  return linkMonitor_->setNodeOverload(true);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetNodeOverload() {
  CHECK(linkMonitor_);
  return linkMonitor_->setNodeOverload(false);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setInterfaceOverload(std::move(*interfaceName), true);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceOverload(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setInterfaceOverload(std::move(*interfaceName), false);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setInterfaceMetric(
    std::unique_ptr<std::string> interfaceName, int32_t overrideMetric) {
  CHECK(linkMonitor_);
  return linkMonitor_->setLinkMetric(std::move(*interfaceName), overrideMetric);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetInterfaceMetric(
    std::unique_ptr<std::string> interfaceName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setLinkMetric(std::move(*interfaceName), std::nullopt);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName,
    int32_t overrideMetric) {
  CHECK(linkMonitor_);
  return linkMonitor_->setAdjacencyMetric(
      std::move(*interfaceName), std::move(*adjNodeName), overrideMetric);
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_unsetAdjacencyMetric(
    std::unique_ptr<std::string> interfaceName,
    std::unique_ptr<std::string> adjNodeName) {
  CHECK(linkMonitor_);
  return linkMonitor_->setAdjacencyMetric(
      std::move(*interfaceName), std::move(*adjNodeName), std::nullopt);
}

folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
OpenrCtrlHandler::semifuture_getInterfaces() {
  CHECK(linkMonitor_);
  return linkMonitor_->getInterfaces();
}

folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
OpenrCtrlHandler::semifuture_getLinkMonitorAdjacencies() {
  CHECK(linkMonitor_);
  return linkMonitor_->getAdjacencies();
}

//
// ConfigStore API
//
folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setConfigKey(
    std::unique_ptr<std::string> key, std::unique_ptr<std::string> value) {
  CHECK(configStore_);
  return configStore_->store(std::move(*key), std::move(*value));
}

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_eraseConfigKey(std::unique_ptr<std::string> key) {
  CHECK(configStore_);
  auto sf = configStore_->erase(std::move(*key));
  return std::move(sf).defer([](folly::Try<bool>&&) { return folly::Unit(); });
}

folly::SemiFuture<std::unique_ptr<std::string>>
OpenrCtrlHandler::semifuture_getConfigKey(std::unique_ptr<std::string> key) {
  CHECK(configStore_);
  auto sf = configStore_->load(std::move(*key));
  return std::move(sf).defer(
      [](folly::Try<std::optional<std::string>>&& val) mutable {
        if (val.hasException() or not val->has_value()) {
          throw thrift::OpenrError("key doesn't exists");
        }
        return std::make_unique<std::string>(std::move(val).value().value());
      });
}

//
// RibPolicy APIs
//

folly::SemiFuture<folly::Unit>
OpenrCtrlHandler::semifuture_setRibPolicy(
    std::unique_ptr<thrift::RibPolicy> policy) {
  return decision_->setRibPolicy(*policy);
}

folly::SemiFuture<std::unique_ptr<thrift::RibPolicy>>
OpenrCtrlHandler::semifuture_getRibPolicy() {
  return decision_->getRibPolicy().deferValue([](thrift::RibPolicy&& policy) {
    return std::make_unique<thrift::RibPolicy>(policy);
  });
}

} // namespace openr
