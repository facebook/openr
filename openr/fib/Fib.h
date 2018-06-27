    /**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/service/stats/ThreadData.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp/async/TAsyncSocket.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/ExponentialBackoff.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <openr/if/gen-cpp2/Fib_types.h>
#include <openr/if/gen-cpp2/LinkMonitor_types.h>
#include <openr/if/gen-cpp2/Platform_types.h>

namespace openr {

/**
 * Proxy agent to program computed routes using platform dependent agent (e.g.
 * FBOSS in case of Wedge Platform).
 *
 * The functionality is very simple. We just react to RouteDatabase updates
 * from Decision module and forward best paths to switch agent to program.
 * There is no state keeping being done apart from handling interface events.
 *
 * On interface event down event we find affected routes and either withdraw
 * them or reprogram with new nexthops.
 *
 * This RouteDatabase contains all Loop Free Alternate (LFAs) paths along with
 * best paths. So Fib module derives best paths (path with minimum cost) and
 * programs them.
 *
 * Note: If for a prefix there are multiple paths with the smallest cost then
 * we program all of them which simulates ECMP behaviours across programmed
 * nexthops.
 *
 */
class Fib final : public fbzmq::ZmqEventLoop {
 public:
  Fib(std::string myNodeName,
      int32_t thriftPort,
      bool dryrun,
      bool enableFibSync,
      std::chrono::seconds coldStartDuration,
      const DecisionPubUrl& decisionPubUrl,
      const FibCmdUrl& fibRepUrl,
      const LinkMonitorGlobalPubUrl& linkMonPubUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  /**
   * Utility function to create thrift client connection to SwitchAgent. Can
   * throw exception if it fails to open transport to client on specified port.
   * It will return immediately if healthy client connection already exists.
   */
  static void createFibClient(
    folly::EventBase& evb,
    std::shared_ptr<apache::thrift::async::TAsyncSocket>& socket,
    std::unique_ptr<thrift::FibServiceAsyncClient>& client,
    int32_t port);

 private:
  // No-copy
  Fib(const Fib&) = delete;
  Fib& operator=(const Fib&) = delete;

  // Initializes ZMQ sockets
  void prepare() noexcept;

  /**
   * Process new route updates received from Decision module
   */
  void processRouteDb(thrift::RouteDatabase&& routeDb);

  /**
   * Process interface status information from LinkMonitor. We remove all
   * routes associated with interface if we detect that it just went down.
   */
  void processInterfaceDb(thrift::InterfaceDatabase&& interfaceDb);

  /**
   * Convert local perfDb_ into PerfDataBase
   */
  thrift::PerfDatabase dumpPerfDb() const;

  /**
   * Trigger add/del routes thrift calls
   * on success no action needed
   * on failure invokes syncRouteDbDebounced
   */
  void updateRoutes(
    const std::vector<thrift::UnicastRoute>& affectedRoutes,
    const std::vector<thrift::IpPrefix>& prefixesToRemove);

  /**
   * Sync the current routeDb_ with the switch agent.
   * on success no action needed
   * on failure invokes syncRouteDbDebounced
   */
  bool syncRouteDb();

  /**
   * Asynchrounsly schedules the syncRouteDb call and returns immediately. All
   * APIs should call this function to sync-routes.
   */
  void syncRouteDbDebounced();

  /**
   * Get aliveSince from FibService, and check if Fib restarts
   * If so, push syncFib to FibService
   */
  void keepAliveCheck();

  // Submit internal state counters to monitor
  void submitCounters();

  // submit events to monitor
  void logEvent(const std::string& event);

  // log perf events
  void logPerfEvents();

  // Prefix to available nexthop information. Also store perf information of
  // received route-db if provided.
  folly::Optional<thrift::PerfEvents> maybePerfEvents_;
  thrift::RouteDatabase routeDb_;
  std::deque<thrift::PerfEvents> perfDb_;

  // Flag to indicate the result of previous route programming attempt.
  // If set, it means what currently cached in local routeDb_ has not been 100%
  // successfully synced with agent, we have to trigger an enforced full fib
  // sync with agent again
  bool dirtyRouteDb_{false};

  // Create timestamp of recently logged perf event
  int64_t recentPerfEventCreateTs_{0};

  // Interface status map
  std::unordered_map<std::string /* ifName*/, bool /* isUp */>
      interfaceStatusDb_;

  // Name of node on which OpenR is running
  const std::string myNodeName_;

  // Switch agent thrift server port
  const int32_t thriftPort_;

  // In dry run we do not make actual thrift call to manipulate routes
  bool dryrun_{true};

  // Enable periodic syncFib to FibAgent
  bool enableFibSync_{false};

  // amount of time to wait before send routes to agent either when this module
  // starts or the agent we are talking with restarts
  const std::chrono::seconds coldStartDuration_;

  // ZMQ sockets for communication with Decision and LinkMonitor modules
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> decisionSub_;
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> fibRep_;
  fbzmq::Socket<ZMQ_SUB, fbzmq::ZMQ_CLIENT> linkMonSub_;

  // ZMQ socket urls
  const std::string decisionPubUrl_;
  const std::string fibRepUrl_;
  const std::string linkMonPubUrl_;

  apache::thrift::CompactSerializer serializer_;

  // Thrift client connection to switch FIB Agent using which we actually
  // manipulate routes.
  folly::EventBase evb_;
  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_{nullptr};
  std::unique_ptr<thrift::FibServiceAsyncClient> client_{nullptr};

  // Callback timer to sync routes to switch agent and scheduled on route-sync
  // failure. ExponentialBackoff timer to ease up things if they go wrong
  std::unique_ptr<fbzmq::ZmqTimeout> syncRoutesTimer_{nullptr};
  ExponentialBackoff<std::chrono::milliseconds> expBackoff_;

  // periodically send alive msg to switch agent
  std::unique_ptr<fbzmq::ZmqTimeout> healthChecker_{nullptr};

  // periodically send sync msg to switch agent
  std::unique_ptr<fbzmq::ZmqTimeout> syncFibTimer_{nullptr};

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

  // Latest aliveSince heard from FibService. If the next one is different then
  // it means that FibAgent has restarted and we need to perform sync.
  int64_t latestAliveSince_{0};

  const int16_t kFibId_{static_cast<int16_t>(thrift::FibClient::OPENR)};
};

} // namespace openr
