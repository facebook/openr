/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <folly/futures/Future.h>

#include <openr/common/AsyncThrottle.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/messaging/Queue.h>

namespace openr {

class PrefixManager final : public OpenrEventBase {
 public:
  struct PrefixEntry; // Forward declaration

  PrefixManager(
      // producer queue
      messaging::ReplicateQueue<thrift::RouteDatabaseDelta>&
          staticRoutesUpdateQueue,
      // consumer queue
      messaging::RQueue<PrefixEvent> prefixUpdatesQueue,
      messaging::RQueue<DecisionRouteUpdate> decisionRouteUpdatesQueue,
      // config
      std::shared_ptr<const Config> config,
      // raw ptr for modules
      KvStore* kvStore,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      const std::chrono::seconds& initialDumpTime);

  ~PrefixManager();

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  // disable copying
  PrefixManager(PrefixManager const&) = delete;
  PrefixManager& operator=(PrefixManager const&) = delete;

  /*
   * Public API for PrefixManager operations:
   *
   * Write APIs - will schedule syncKvStoreThrottled_ to update kvstore,
   * @return true if there are changes else false
   *  - add prefixes
   *  - withdraw prefixes
   *  - withdraw prefixes by type
   *  - sync prefixes by type: replace all prefixes of @type w/ @prefixes
   *
   *
   * Read APIs - dump internal prefixDb
   *  - dump all prefixes
   *  - dump all prefixes by type
   */
  folly::SemiFuture<bool> advertisePrefixes(
      std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<bool> withdrawPrefixes(
      std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<bool> withdrawPrefixesByType(thrift::PrefixType prefixType);

  folly::SemiFuture<bool> syncPrefixesByType(
      thrift::PrefixType prefixType, std::vector<thrift::PrefixEntry> prefixes);

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  getPrefixes();

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::PrefixEntry>>>
  getPrefixesByType(thrift::PrefixType prefixType);

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::AdvertisedRouteDetail>>>
  getAdvertisedRoutesFiltered(thrift::AdvertisedRouteFilter filter);

  folly::SemiFuture<std::unique_ptr<std::vector<thrift::OriginatedPrefixEntry>>>
  getOriginatedPrefixes();

  /**
   * Filter routes only the <type> attribute
   */
  static void filterAndAddAdvertisedRoute(
      std::vector<thrift::AdvertisedRouteDetail>& routes,
      apache::thrift::optional_field_ref<thrift::PrefixType&> const& typeFilter,
      thrift::IpPrefix const& prefix,
      std::unordered_map<thrift::PrefixType, PrefixEntry> const& prefixEntries);

  // prefix entry with their destination areas
  // if dstAreas become empty, entry should be withdrawn
  struct PrefixEntry {
    thrift::PrefixEntry tPrefixEntry;
    std::unordered_set<std::string> dstAreas;

    PrefixEntry() = default;
    template <typename TPrefixEntry, typename AreaSet>
    PrefixEntry(TPrefixEntry&& tPrefixEntry, AreaSet&& dstAreas)
        : tPrefixEntry(std::forward<TPrefixEntry>(tPrefixEntry)),
          dstAreas(std::forward<AreaSet>(dstAreas)) {}

    apache::thrift::field_ref<const thrift::PrefixMetrics&>
    metrics_ref() const& {
      return tPrefixEntry.metrics_ref();
    }

    bool
    operator==(const PrefixEntry& other) const {
      return tPrefixEntry == other.tPrefixEntry && dstAreas == other.dstAreas;
    }
  };

 private:
  /*
   * Private helpers to update prefixMap_ and send prefixes to KvStore
   *
   * Called upon:
   * - public write APIs
   * - request from replicate queue
   *
   * modify prefix db and schedule syncKvStoreThrottled_ to update kvstore
   * @return true if the db is modified
   */
  bool advertisePrefixesImpl(
      const std::vector<thrift::PrefixEntry>& prefixes,
      const std::unordered_set<std::string>& dstAreas);
  bool advertisePrefixesImpl(const std::vector<PrefixEntry>& prefixes);
  bool withdrawPrefixesImpl(const std::vector<thrift::PrefixEntry>& prefixes);
  bool withdrawPrefixesByTypeImpl(thrift::PrefixType type);
  bool syncPrefixesByTypeImpl(
      thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes,
      const std::unordered_set<std::string>& dstAreas);

  // Util function to interact KvStore to inject/withdraw keys
  void syncKvStore();

  /*
   * [Route Origination/Aggregation]
   *
   * Methods implement utility function for route origination/aggregation.
   */

  /*
   * Read routes from OpenrConfig and stored in `OriginatedPrefixDb_`
   * for potential advertisement/withdrawn based on min_supporting_route
   * ref-count.
   */
  void buildOriginatedPrefixDb(
      const std::vector<thrift::OriginatedPrefix>& prefixes);

  /*
   * Util function to process ribEntry update from `Decision` and populate
   * aggregates to advertise
   */
  void aggregatesToAdvertise(const folly::CIDRNetwork& prefix);

  /*
   * Util function to process ribEntry update from `Decision` and populate
   * aggregates to withdraw
   */
  void aggregatesToWithdraw(const folly::CIDRNetwork& prefix);

  // Inject `PrefixEntry` into dstAreas.
  // Return list of prefixes for each injected area.
  std::vector<std::string> updateKvStorePrefixEntry(PrefixEntry const& entry);

  // process decision route update, inject routes to different areas
  void processDecisionRouteUpdates(DecisionRouteUpdate&& decisionRouteUpdate);

  // add event named updateEvent to perfEvents if it has value and the last
  // element is not already updateEvent
  void addPerfEventIfNotExist(
      thrift::PerfEvents& perfEvents, std::string const& updateEvent);

  // this node name
  const std::string nodeId_;

  // queue to publish originated route updates to decision
  messaging::ReplicateQueue<thrift::RouteDatabaseDelta>&
      staticRouteUpdatesQueue_;

  // module ptr to interact with KvStore
  KvStore* kvStore_{nullptr};

  // enable convergence performance measurement for Adjacencies update
  const bool enablePerfMeasurement_{false};

  // Throttled version of syncKvStore. It batches up multiple calls and
  // send them in one go!
  std::unique_ptr<AsyncThrottle> syncKvStoreThrottled_;
  std::unique_ptr<folly::AsyncTimeout> initialSyncKvStoreTimer_;

  // TTL for a key in the key value store
  const std::chrono::milliseconds ttlKeyInKvStore_;

  // kvStoreClient for persisting our prefix db
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_{nullptr};

  // The current prefix db this node is advertising. In-case if multiple entries
  // exists for a given prefix, best-route-selection process would select the
  // ones with the best metric. Lowest prefix-type is used as a tie-breaker for
  // advertising the best selected routes to KvStore.
  std::unordered_map<
      thrift::IpPrefix,
      std::unordered_map<thrift::PrefixType, PrefixEntry>>
      prefixMap_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // collection to track prefixes to be withdrawn.
  // ATTN: this collection is maintained to easily find the prefix delta
  //       between current/previous advertisement.
  std::unordered_set<std::string> keysToClear_;

  // perfEvents related to a given prefixEntry
  std::unordered_map<
      thrift::PrefixType,
      std::unordered_map<thrift::IpPrefix, thrift::PerfEvents>>
      addingEvents_;

  // area Id
  const std::unordered_set<std::string> allAreas_{};

  // TODO:
  //   struct AreaInfo {
  //     // ingress policy
  //     // AreaPolicy ingressPolicy;
  //     // store post policy prefix entries
  //     std::unordered_map<folly::CIDRNetwork, thrift::PrefixEntry>
  //         postPolicyPrefixes;
  //   }

  //   std::unordered_map<std::string, AreaInfo> areaInfos_;

  /*
   * [Route Origination/Aggregation]
   *
   * Local-originated prefixes will be advertise/withdrawn from
   * `Prefix-Manager` by calculating ref-count of supporting-
   * route from `Decision`.
   *  --------               ---------
   *           ------------>
   *  Decision               PrefixMgr
   *           <------------
   *  --------               ---------
   */

  // struct to represent local-originiated route
  struct OriginatedRoute {
    // thrift structure read from config
    thrift::OriginatedPrefix originatedPrefix;

    // unicastEntry contains nexthops info
    RibUnicastEntry unicastEntry;

    // supporting routes for this originated prefix
    std::unordered_set<folly::CIDRNetwork> supportingRoutes{};

    // flag indicates is local route has been originated
    bool isAdvertised{false};

    OriginatedRoute(
        const thrift::OriginatedPrefix& originatedPrefix,
        const RibUnicastEntry& unicastEntry,
        const std::unordered_set<folly::CIDRNetwork>& supportingRoutes)
        : originatedPrefix(originatedPrefix),
          unicastEntry(unicastEntry),
          supportingRoutes(supportingRoutes) {}
  };

  /*
   * prefixes to be originated from prefix-manager
   * ATTN: to support quick information retrieval, cache the mapping:
   *
   *  OriginatedPrefix -> set of RIB prefixEntry(i.e. supporting routes)
   */
  std::unordered_map<folly::CIDRNetwork, OriginatedRoute> originatedPrefixDb_;

  /*
   * prefixes received from decision
   * ATTN: to avoid loop through ALL entries inside `originatedPrefixes`,
   *       cache the reverse mapping:
   *
   *  RIB prefixEntry -> vector of OriginatedPrefix(i.e. subnet)
   */
  std::unordered_map<folly::CIDRNetwork, std::vector<folly::CIDRNetwork>>
      ribPrefixDb_;
}; // PrefixManager

} // namespace openr
