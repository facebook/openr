/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqThrottle.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/ExponentialBackoff.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/Decision_types.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {
struct ProcessPublicationResultOld {
  bool adjChanged{false};
  bool prefixesChanged{false};
};

namespace detail {
/**
 * Keep track of hash for pending SPF calculation because of certain
 * updates in graph.
 * Out of all buffered applications we try to keep the perf events for the
 * oldest appearing event.
 */
struct DecisionOldPendingUpdates {
  void
  clear() {
    count_ = 0;
    minTs_ = folly::none;
    perfEvents_ = folly::none;
  }

  void
  addUpdate(
      const std::string& nodeName,
      const folly::Optional<thrift::PerfEvents>& perfEvents) {
    ++count_;

    // Skip if perf information is missing
    if (not perfEvents.hasValue()) {
      return;
    }

    // Update local copy of perf evens if it is newer than the one to be added
    // We do debounce (batch updates) for recomputing routes and in order to
    // measure convergence performance, it is better to use event which is
    // oldest.
    if (!minTs_ or minTs_.value() > perfEvents->events.front().unixTs) {
      minTs_ = perfEvents->events.front().unixTs;
      perfEvents_ = perfEvents;
      addPerfEvent(*perfEvents_, nodeName, "DECISION_RECEIVED");
    }
  }

  uint32_t
  getCount() const {
    return count_;
  }

  folly::Optional<thrift::PerfEvents>
  getPerfEvents() const {
    return perfEvents_;
  }

 private:
  uint32_t count_{0};
  folly::Optional<int64_t> minTs_;
  folly::Optional<thrift::PerfEvents> perfEvents_;
};
} // namespace detail

// The class to compute shortest-paths using Dijkstra algorithm
class SpfSolverOld {
 public:
  // these need to be defined in the .cpp so they can refer
  // to the actual implementation of SpfSolverOldImpl
  explicit SpfSolverOld(bool enableV4);
  ~SpfSolverOld();

  //
  // The following methods talk to implementation so need to
  // be defined in the .cpp
  //

  // update adjacencies for the given router; everything is replaced. Returns
  // true if this has caused any change in graph
  bool updateAdjacencyDatabase(thrift::AdjacencyDatabase const& adjacencyDb);

  // delete a node's adjacency database
  // return true if this has caused any change in graph
  bool deleteAdjacencyDatabase(const std::string& nodeName);

  // get adjacency databases
  std::unordered_map<std::string /* nodeName */, thrift::AdjacencyDatabase>
  getAdjacencyDatabases();

  // update prefixes for a given router. Returns true if this has caused any
  // routeDb change
  bool updatePrefixDatabase(thrift::PrefixDatabase const& prefixDb);

  // delete a node's prefix database
  // return true if this has caused any change in routeDb
  bool deletePrefixDatabase(const std::string& nodeName);

  // get prefix databases
  std::unordered_map<std::string /* nodeName */, thrift::PrefixDatabase>
  getPrefixDatabases();

  // compute the routes from perspective of a given router
  thrift::RouteDatabase buildShortestPaths(const std::string& myNodeName);

  // compute all LFA routes from perspective of a given router
  thrift::RouteDatabase buildMultiPaths(const std::string& myNodeName);

  // build route database using global prefix database and cached SPF
  // computation from perspective of a given router.
  thrift::RouteDatabase buildRouteDb(const std::string& myNodeName);

  std::unordered_map<std::string, int64_t> getCounters();

 private:
  // no-copy
  SpfSolverOld(SpfSolverOld const&) = delete;
  SpfSolverOld& operator=(SpfSolverOld const&) = delete;

  // pointer to implementation class
  class SpfSolverOldImpl;
  std::unique_ptr<SpfSolverOldImpl> impl_;
};

//
// The decision thread announces FIB updates for myNodeName every time
// there is a change in LSDB. The announcements are made on a PUB socket. At
// the same time, it listens on a REP socket to respond with the recent
// FIB state if requested by clients.
//
// On the "client" side of things, it uses REQ socket to request a full dump
// of link-state information from KvStore, and before that it subscribes to
// the PUB address of the KvStore to receive ongoing LSDB updates from KvStore.
//
// The prefix/adjacency Db markers are used to find the keys in KvStore that
// correspond to the prefix information or link state information. This way
// we do not need to try and parse the values to tell that. For example,
// the key name could be "adj:router1" or "prefix:router2" to tell of
// the AdjacencyDatabase of router1 and PrefixDatabase of router2
//

class DecisionOld : public fbzmq::ZmqEventLoop {
 public:
  DecisionOld(
      std::string myNodeName,
      bool enableV4,
      const AdjacencyDbMarker& adjacencyDbMarker,
      const PrefixDbMarker& prefixDbMarker,
      std::chrono::milliseconds debounceMinDur,
      std::chrono::milliseconds debounceMaxDur,
      const KvStoreLocalCmdUrl& storeCmdUrl,
      const KvStoreLocalPubUrl& storePubUrl,
      const DecisionCmdUrl& decisionCmdUrl,
      const DecisionPubUrl& decisionPubUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  virtual ~DecisionOld() = default;

  std::unordered_map<std::string, int64_t> getCounters();

 private:
  DecisionOld(DecisionOld const&) = delete;
  DecisionOld& operator=(DecisionOld const&) = delete;

  void prepare(fbzmq::Context& zmqContext) noexcept;

  // process request
  void processRequest();

  // process publication from KvStore
  ProcessPublicationResultOld processPublication(
      thrift::Publication const& thriftPub);

  /**
   * Process received publication and populate the pendingAdjUpdates_
   * attributes which can be applied later on after a debounce timeout.
   */
  detail::DecisionOldPendingUpdates pendingAdjUpdates_;

  /**
   * Process received publication and populate the pendingPrefixUpdates_
   * attributes upon receiving prefix update publication
   */
  detail::DecisionOldPendingUpdates pendingPrefixUpdates_;

  /**
   * Timer to schedule pending update processing
   * Refer to processUpdatesStatus_ to decide whether spf recalculation or
   * just route rebuilding is needed.
   * Apply exponential backoff timeout to avoid churn
   */
  std::unique_ptr<fbzmq::ZmqTimeout> processUpdatesTimer_;
  ExponentialBackoff<std::chrono::milliseconds> processUpdatesBackoff_;

  // store update to-do status
  ProcessPublicationResultOld processUpdatesStatus_;

  /**
   * Caller function of processPendingAdjUpdates and processPendingPrefixUpdates
   * Check current processUpdatesStatus_ to decide which sub function to call
   * to further process pending updates
   * Reset timer and status afterwards.
   */
  void processPendingUpdates();

  /**
   * Function to process pending adjacency publications.
   */
  void processPendingAdjUpdates();

  /**
   * Function to process prefix updates.
   */
  void processPendingPrefixUpdates();

  // perform full dump of all LSDBs and run initial routing computations
  void initialSync(fbzmq::Context& zmqContext);

  // periodically submit counters to monitor thread
  void submitCounters();

  // submit events to monitor
  void logRouteEvent(const std::string& event, const int numOfRoutes);
  void logDebounceEvent(
      const int numUpdates, const std::chrono::milliseconds debounceTime);

  // this node's name and the key markers
  const std::string myNodeName_;
  // the prefix we use to find the adjacency database announcements
  const std::string adjacencyDbMarker_;
  // the prefix we use to find the prefix db key announcements
  const std::string prefixDbMarker_;

  // URLs for the sockets
  const std::string storeCmdUrl_;
  const std::string storePubUrl_;
  const std::string decisionCmdUrl_;
  const std::string decisionPubUrl_;

  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> storeSub_;
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> decisionRep_;
  fbzmq::Socket<ZMQ_PUB, fbzmq::ZMQ_SERVER> decisionPub_;

  // the pointer to the SPF path calculator
  std::unique_ptr<SpfSolverOld> spfSolver_;

  apache::thrift::CompactSerializer serializer_;

  // base interval to submit to monitor with (jitter will be added)
  std::chrono::seconds monitorSyncInterval_{0};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;
};

} // namespace openr
