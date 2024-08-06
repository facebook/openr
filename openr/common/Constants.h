/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <list>
#include <string>

#include <folly/IPAddress.h>

namespace openr {

using namespace std::chrono_literals;

class Constants {
 public:
  /*
   * [Initialization] Constants used for Open/R Initialization procedure
   */

  // Upper-limit time duration of LINK_DISCOVER stage
  static constexpr std::chrono::seconds kMaxDurationLinkDiscovery{10};

  // TODO: we may consider define upper-limit for each stage of initialization

  //
  // Common
  //

  // this is the maximum time we wait for read data on a socket
  // this is an important constant, as we do not implement any
  // recovery from read errors. We expect in our network reads
  // to be "fast" since we talk to directly adjacent nodes
  static constexpr std::chrono::milliseconds kReadTimeout{1000};

  static constexpr auto kInitEventCounterFormat =
      "initialization.{}.duration_ms";

  // default interval to publish to monitor
  static constexpr std::chrono::seconds kCounterSubmitInterval{5};

  // event log category
  static constexpr folly::StringPiece kEventLogCategory{"perfpipe_aquaman"};

  /*
   * [Exponential Backoff Constants]
   */

  // Link-monitor
  static constexpr std::chrono::milliseconds kInitialBackoff{64};
  static constexpr std::chrono::milliseconds kMaxBackoff{8192};

  // Kvstore
  static constexpr std::chrono::milliseconds kKvstoreSyncInitialBackoff{4s};
  static constexpr std::chrono::milliseconds kKvstoreSyncMaxBackoff{256s};
  static constexpr auto kKvStoreNumPeerByStateCounter =
      "kvstore.num_peers.{}.{}";

  // 0 if there is at least 1 peer in initiliazed state
  // 1 if all peers are NOT in initialized state
  static constexpr auto kKvStoreAllPeerNotInitialized =
      "kvstore.all_peers_not_initialized.{}";

  // regex ends with .p###
  static constexpr auto podEndingPattern = ".*\\.(p)\\d{3}$";
  // regex ends with .s###
  static constexpr auto spineEndingPattern = ".*\\.(s)\\d{3}$";

  // regex ends with .slice###
  static constexpr auto sliceEndingPattern = ".*\\.(slice)\\d{3}$";

  // regex ends with .fa###
  static constexpr auto hgridEndingPattern = ".*\\.(fa)\\d{3}$";

  // Fib
  static constexpr std::chrono::milliseconds kFibInitialBackoff{8};
  static constexpr std::chrono::milliseconds kFibMaxBackoff{4096};

  // Persistent-Store
  static constexpr std::chrono::milliseconds kPersistentStoreInitialBackoff{
      100};
  static constexpr std::chrono::milliseconds kPersistentStoreMaxBackoff{5000};

  /*
   * [LinkMonitor Constants]
   */

  // Hold time to wait before advertising link events. We use different
  // timers for UP (Throttle=100ms) and DOWN events are immediately
  static constexpr std::chrono::milliseconds kLinkThrottleTimeout{100};
  static constexpr std::chrono::milliseconds kLinkImmediateTimeout{1};

  // Hold time to wait before advertising adjacency UP event to KvStore.
  // Adjacency DOWN event is immediately advertised.
  static constexpr std::chrono::milliseconds kAdjacencyThrottleTimeout{1000};

  /*
   * [Spark Constants]
   */

  // the multicast address used by Spark
  static constexpr folly::StringPiece kSparkMcastAddr{"ff02::1"};

  // The maximum number of spark packets per second we will process from
  // a iface, ip addr pairs that hash to the same bucket in our
  // fixed size list of BucketedTimeSeries
  static constexpr uint32_t kMaxAllowedPps{50};

  // Number of BucketedTimeSeries to spread potential neighbors across
  // for the purpose of limiting the number of packets per second processed
  static constexpr size_t kNumTimeSeries{1024};

  /*
   * [Platform/Fib Constants]
   */

  // Default const parameters for Open/R -> Platform agent thrift connection
  static constexpr folly::StringPiece kPlatformHost{"::1"};
  static constexpr std::chrono::milliseconds kPlatformConnTimeout{100};
  static constexpr std::chrono::milliseconds kPlatformProcTimeout{10000}; // 10s

  // Default const parameters for external entity -> Open/R thrift connection
  static constexpr std::chrono::milliseconds kServiceConnTimeout{500};
  static constexpr std::chrono::milliseconds kServiceConnSSLTimeout{1000};
  static constexpr std::chrono::milliseconds kServiceProcTimeout{2500};

  // Time interval to sync between Open/R and Platform
  static constexpr std::chrono::seconds kPlatformSyncInterval{60};

  // Time interval for keep alive check between fib and switch agent
  static constexpr std::chrono::milliseconds kKeepAliveCheckInterval{1000};

  /*
   * Timeout duration for which if a client connection has no activity, then it
   * will be dropped.
   *
   * Attention: this flag only applies when Open/R starts thrift server to
   * accept thrift requests on its own.
   */
  static constexpr std::chrono::seconds kPlatformThriftIdleTimeout{
      Constants::kPlatformSyncInterval * 3};

  /*
   * [KvStore Constants]
   */

  // Default interval for flooding topology dump
  static constexpr std::chrono::seconds kFloodTopoDumpInterval{300}; // 5min

  // Default thrift client keep alive interval to avoid idle timeout
  static constexpr std::chrono::seconds kThriftClientKeepAliveInterval{20};

  // Count of maximum pending kvstore sync response before waiting for
  // kMaxBackoff to send the next sync request
  static constexpr size_t kMaxFullSyncPendingCountThreshold{32};

  // If version is undefined, the corresponding thrift::Value is invalid.
  static constexpr int64_t kUndefinedVersion{0};

  // the time we hold on to clear keys from KvStore
  static constexpr std::chrono::milliseconds kKvStoreClearThrottleTimeout{10};

  // the time we hold on to announce to KvStore
  static constexpr std::chrono::milliseconds kKvStoreSyncThrottleTimeout{100};

  // Kvstore timer for flooding pending publication
  static constexpr std::chrono::milliseconds kFloodPendingPublication{100};

  // delimiter separating prefix and name in kvstore key
  static constexpr folly::StringPiece kPrefixNameSeparator{":"};

  // NOTE: this is a special areaId that is treated as the wildcard.
  // Interfaces configured into this area will form adjacencies with any other
  // node not validating what area they claim to be in.
  static constexpr folly::StringPiece kDefaultArea{"0"};

  // KvStore key markers
  static constexpr folly::StringPiece kAdjDbMarker{"adj:"};
  static constexpr folly::StringPiece kPrefixDbMarker{"prefix:"};

  static constexpr folly::StringPiece kOpenrCtrlSessionContext{"OpenrCtrl"};

  // max interval to update TTL for each key in kvstore w/ finite TTL
  static constexpr std::chrono::milliseconds kMaxTtlUpdateInterval{2h};

  // TTL infinity, never expires
  static constexpr int64_t kTtlInfinity{INT32_MIN}; // integer version
  static constexpr std::chrono::milliseconds kTtlInfInterval{kTtlInfinity};

  // Ttl to decrement before re-flooding
  static constexpr std::chrono::milliseconds kTtlDecrement{1};
  // Min ttl expiry time to qualify for peer sync
  static constexpr std::chrono::milliseconds kTtlThreshold{500};

  // adjacencies can have weights for weighted ecmp
  static constexpr int64_t kDefaultAdjWeight{1};

  // buffer size to keep latest perf log
  static constexpr uint16_t kPerfBufferSize{10};
  static constexpr std::chrono::seconds kConvergenceMaxDuration{3s};

  // hold time for longPoll requests in openrCtrl thrift server
  static constexpr std::chrono::milliseconds kLongPollReqHoldTime{20000};

  //
  // Prefix manager specific
  //

  // Default metrics (path and source preference) for Open/R originated routes
  // (loopback address & interface subnets).
  static constexpr int32_t kDefaultPathPreference{1000}; // LIVE routes
  static constexpr int32_t kDefaultSourcePreference{200}; // Source pref

  // Nexthops used to program drop route
  static constexpr folly::StringPiece kLocalRouteNexthopV4{"0.0.0.0"};
  static constexpr folly::StringPiece kLocalRouteNexthopV6{"::"};

  // Openr Ctrl thrift server port
  static constexpr int32_t kOpenrCtrlPort{2018};

  // Thrift server's queue timeout
  static constexpr std::chrono::milliseconds kThriftServerQueueTimeout{1000};

  //
  // See https://github.com/facebook/openr/blob/master/openr/docs/Versions.md
  // for details of version history
  //
  // ATTN: Please update `Versions.md` when current Open/R version
  //       is bumped up.
  //

  // Current OpenR version
  static constexpr int32_t kOpenrVersion{20200825};

  // Lowest Supported OpenR version
  static constexpr int32_t kOpenrSupportedVersion{20200604};

  // Threshold time in secs to crash after reaching critical memory
  static constexpr std::chrono::seconds kMemoryThresholdTime{600};
};

} // namespace openr
