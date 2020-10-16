/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb303/BaseService.h>
#include <openr/common/Types.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/decision/Decision.h>
#include <openr/fib/Fib.h>
#include <openr/if/gen-cpp2/OpenrCtrlCpp.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStorePublisher.h>
#include <openr/link-monitor/LinkMonitor.h>
#include <openr/monitor/Monitor.h>
#include <openr/prefix-manager/PrefixManager.h>

namespace openr {
class OpenrCtrlHandler final : public thrift::OpenrCtrlCppSvIf,
                               public facebook::fb303::BaseService {
 public:
  /**
   * NOTE: If acceptablePeerCommonNames is empty then check for peerName is
   *       skipped
   */
  OpenrCtrlHandler(
      const std::string& nodeName,
      const std::unordered_set<std::string>& acceptablePeerCommonNames,
      OpenrEventBase* ctrlEvb,
      Decision* decision,
      Fib* fib,
      KvStore* kvStore,
      LinkMonitor* linkMonitor,
      Monitor* Monitor,
      PersistentStore* configStore,
      PrefixManager* prefixManager,
      Spark* spark,
      std::shared_ptr<const Config> config);

  ~OpenrCtrlHandler() override;

  //
  // fb303 service APIs
  //

  facebook::fb303::cpp2::fb303_status getStatus() override;

  void getCounters(std::map<std::string, int64_t>& _return) override;
  void getRegexCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::string> regex) override;
  void getSelectedCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::vector<std::string>> keys) override;
  int64_t getCounter(std::unique_ptr<std::string> key) override;

  // Openr Node Name
  void getMyNodeName(std::string& _return) override;

  //
  // config APIs
  //

  void getRunningConfig(std::string& _return) override;

  void getRunningConfigThrift(thrift::OpenrConfig& _config) override;

  void dryrunConfig(
      ::std::string& _return, std::unique_ptr<::std::string> file) override;

  //
  // Monitor APIs
  //

  void getEventLogs(std::vector<::std::string>& _return) override;

  //
  // PrefixManager APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_advertisePrefixes(
      std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) override;

  folly::SemiFuture<folly::Unit> semifuture_withdrawPrefixes(
      std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) override;

  folly::SemiFuture<folly::Unit> semifuture_withdrawPrefixesByType(
      thrift::PrefixType prefixType) override;

  folly::SemiFuture<folly::Unit> semifuture_syncPrefixesByType(
      thrift::PrefixType prefixType,
      std::unique_ptr<std::vector<thrift::PrefixEntry>> prefixes) override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  semifuture_getPrefixes() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  semifuture_getPrefixesByType(thrift::PrefixType prefixType) override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
  semifuture_getReceivedRoutes() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::ReceivedRouteDetail>>>
  semifuture_getReceivedRoutesFiltered(
      std::unique_ptr<thrift::ReceivedRouteFilter> filter) override;

  //
  // Fib APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
  semifuture_getRouteDb() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
  semifuture_getUnicastRoutesFiltered(
      std::unique_ptr<std::vector<::std::string>> prefixes) override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::UnicastRoute>>>
  semifuture_getUnicastRoutes() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::MplsRoute>>>
  semifuture_getMplsRoutesFiltered(
      std::unique_ptr<std::vector<int32_t>> labels) override;

  folly::SemiFuture<std::unique_ptr<std::vector<openr::thrift::MplsRoute>>>
  semifuture_getMplsRoutes() override;

  //
  // Spark APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_floodRestartingMsg() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::SparkNeighbor>>>
  semifuture_getNeighbors() override;

  //
  // Performance stats APIs
  //

  folly::SemiFuture<std::unique_ptr<thrift::PerfDatabase>>
  semifuture_getPerfDb() override;

  //
  // Decision APIs
  //

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
  semifuture_getAdvertisedRoutes() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
  semifuture_getAdvertisedRoutesFiltered(
      std::unique_ptr<thrift::AdvertisedRouteFilter> filter) override;

  folly::SemiFuture<std::unique_ptr<thrift::AdjDbs>>
  semifuture_getDecisionAdjacencyDbs() override;

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdjacencyDatabase>>>
  semifuture_getAllDecisionAdjacencyDbs() override;

  folly::SemiFuture<std::unique_ptr<thrift::PrefixDbs>>
  semifuture_getDecisionPrefixDbs() override;

  folly::SemiFuture<std::unique_ptr<thrift::RouteDatabase>>
  semifuture_getRouteDbComputed(std::unique_ptr<std::string> nodeName) override;

  //
  // KvStore APIs
  //
  folly::SemiFuture<std::unique_ptr<thrift::AreasConfig>>
  semifuture_getAreasConfig() override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyVals(
      std::unique_ptr<std::vector<std::string>> filterKeys) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsArea(
      std::unique_ptr<std::vector<std::string>> filterKeys,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsFiltered(
      std::unique_ptr<thrift::KeyDumpParams> filter) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreHashFiltered(
      std::unique_ptr<thrift::KeyDumpParams> filter) override;

  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreHashFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> setParams,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<folly::Unit> semifuture_processKvStoreDualMessage(
      std::unique_ptr<thrift::DualMessages> messages,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<folly::Unit> semifuture_updateFloodTopologyChild(
      std::unique_ptr<thrift::FloodTopoSetParams> params,
      std::unique_ptr<std::string> area) override;

  folly::SemiFuture<std::unique_ptr<thrift::SptInfos>>
  semifuture_getSpanningTreeInfos(std::unique_ptr<std::string> area) override;

  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeers() override;

  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeersArea(std::unique_ptr<std::string> area) override;

  // Stream API's
  // Intentionally not use SemiFuture as stream is async by nature and we will
  // immediately create and return the stream handler
  apache::thrift::ServerStream<thrift::Publication> subscribeKvStoreFilter(
      std::unique_ptr<thrift::KeyDumpParams>);
  apache::thrift::ServerStream<thrift::RouteDatabaseDelta> subscribeFib();

  folly::SemiFuture<apache::thrift::ResponseAndServerStream<
      thrift::Publication,
      thrift::Publication>>
  semifuture_subscribeAndGetKvStore() override;

  folly::SemiFuture<apache::thrift::ResponseAndServerStream<
      thrift::Publication,
      thrift::Publication>>
  semifuture_subscribeAndGetKvStoreFiltered(
      std::unique_ptr<thrift::KeyDumpParams> filter) override;

  folly::SemiFuture<apache::thrift::ResponseAndServerStream<
      thrift::RouteDatabase,
      thrift::RouteDatabaseDelta>>
  semifuture_subscribeAndGetFib() override;

  // Long poll support
  folly::SemiFuture<bool> semifuture_longPollKvStoreAdj(
      std::unique_ptr<thrift::KeyVals> snapshot) override;

  //
  // LinkMonitor APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_setNodeOverload() override;
  folly::SemiFuture<folly::Unit> semifuture_unsetNodeOverload() override;

  folly::SemiFuture<folly::Unit> semifuture_setInterfaceOverload(
      std::unique_ptr<std::string> interfaceName) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceOverload(
      std::unique_ptr<std::string> interfaceName) override;

  folly::SemiFuture<folly::Unit> semifuture_setInterfaceMetric(
      std::unique_ptr<std::string> interfaceName,
      int32_t overrideMetric) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetInterfaceMetric(
      std::unique_ptr<std::string> interfaceName) override;

  folly::SemiFuture<folly::Unit> semifuture_setAdjacencyMetric(
      std::unique_ptr<std::string> interfaceName,
      std::unique_ptr<std::string> adjNodeName,
      int32_t overrideMetric) override;
  folly::SemiFuture<folly::Unit> semifuture_unsetAdjacencyMetric(
      std::unique_ptr<std::string> interfaceName,
      std::unique_ptr<std::string> adjNodeName) override;

  folly::SemiFuture<std::unique_ptr<thrift::DumpLinksReply>>
  semifuture_getInterfaces() override;

  folly::SemiFuture<std::unique_ptr<thrift::AdjacencyDatabase>>
  semifuture_getLinkMonitorAdjacencies() override;

  // Explicitly override blocking API call as no ASYNC needed
  void getOpenrVersion(thrift::OpenrVersions& openrVersion) override;
  void getBuildInfo(thrift::BuildInfo& buildInfo) override;

  //
  // PersistentStore APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_setConfigKey(
      std::unique_ptr<std::string> key,
      std::unique_ptr<std::string> value) override;

  folly::SemiFuture<folly::Unit> semifuture_eraseConfigKey(
      std::unique_ptr<std::string> key) override;

  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_getConfigKey(
      std::unique_ptr<std::string> key) override;

  //
  // RibPolicy APIs
  //

  folly::SemiFuture<folly::Unit> semifuture_setRibPolicy(
      std::unique_ptr<thrift::RibPolicy> policy) override;

  folly::SemiFuture<std::unique_ptr<thrift::RibPolicy>>
  semifuture_getRibPolicy() override;

  //
  // APIs to expose state of private variables
  //

  inline size_t
  getNumKvStorePublishers() {
    return kvStorePublishers_.wlock()->size();
  }

  inline size_t
  getNumPendingLongPollReqs() {
    return longPollReqs_->size();
  }

  inline size_t
  getNumFibPublishers() {
    return fibPublishers_.wlock()->size();
  }

  //
  // API to cleanup private variables
  //
  inline void
  cleanupPendingLongPollReqs() {
    longPollReqs_->clear();
  }

 private:
  void authorizeConnection();
  void closeKvStorePublishers();
  void closeFibPublishers();

  const std::string nodeName_;
  const std::unordered_set<std::string> acceptablePeerCommonNames_;

  // Pointers to Open/R modules
  Decision* decision_{nullptr};
  Fib* fib_{nullptr};
  KvStore* kvStore_{nullptr};
  LinkMonitor* linkMonitor_{nullptr};
  Monitor* monitor_{nullptr};
  PersistentStore* configStore_{nullptr};
  PrefixManager* prefixManager_{nullptr};
  Spark* spark_{nullptr};
  std::shared_ptr<const Config> config_;

  // Publisher token (monotonically increasing) for all publishers
  std::atomic<int64_t> publisherToken_{0};

  // Active kvstore snoop publishers
  folly::Synchronized<
      std::unordered_map<int64_t, std::unique_ptr<KvStorePublisher>>>
      kvStorePublishers_;

  // Active Fib streaming publishers
  folly::Synchronized<std::unordered_map<
      int64_t,
      apache::thrift::ServerStreamPublisher<thrift::RouteDatabaseDelta>>>
      fibPublishers_;

  // pending longPoll requests from clients, which consists of
  // 1). promise; 2). timestamp when req received on server
  std::atomic<int64_t> pendingRequestId_{0};
  folly::Synchronized<
      std::unordered_map<int64_t, std::pair<folly::Promise<bool>, int64_t>>>
      longPollReqs_;

  // fiber task future hold for kvStore update, fib update reader's
  std::vector<folly::Future<folly::Unit>> workers_;
}; // class OpenrCtrlHandler
} // namespace openr
