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

  // ExponentialBackoff durations
  // Link-monitor
  static constexpr std::chrono::milliseconds kInitialBackoff{64};
  static constexpr std::chrono::milliseconds kMaxBackoff{8192};
  // Kvstore
  static constexpr std::chrono::milliseconds kKvstoreSyncInitialBackoff{2000};
  static constexpr std::chrono::milliseconds kKvstoreSyncMaxBackoff{64000};
  // FIB, perhaps this could be removed and above used in time
  static constexpr std::chrono::milliseconds kFibInitialBackoff{8};
  static constexpr std::chrono::milliseconds kFibMaxBackoff{4096};

  // Persistent store specific
  static constexpr std::chrono::milliseconds kPersistentStoreInitialBackoff{
      100};
  static constexpr std::chrono::milliseconds kPersistentStoreMaxBackoff{5000};

  //
  // KvStore specific
  //

  // default interval for flooding topology dump
  static constexpr std::chrono::seconds kFloodTopoDumpInterval{300};

  // default thrift client keep alive interval to avoid idle timeout
  static constexpr std::chrono::seconds kThriftClientKeepAliveInterval{30};

  // Count of maximum pending kvstore sync response before waiting for
  // kMaxBackoff to send the next sync request
  static constexpr size_t kMaxFullSyncPendingCountThreshold{32};

  // Invalid version for thrift::Value
  // If version is undefined, then thrift::Value
  // is not valid
  static constexpr int64_t kUndefinedVersion{0};

  // Timeout for kvstore streaming in milliseconds
  static constexpr int64_t kStreamTimeoutMs{100};
  // Queue timeout for kvstore streaming in milliseconds
  static constexpr int64_t kStreamQueueTimeoutMs{5000};

  //
  // PrefixAllocator specific
  //

  // default interval for prefix allocator to sync with kvstore
  static constexpr std::chrono::milliseconds kPrefixAllocatorSyncInterval{1000};

  // seed prefix and allocated prefix length is separate by comma
  static constexpr folly::StringPiece kSeedPrefixAllocLenSeparator{","};

  // kvstore key for prefix allocator parameters indicating seed prefix and
  // allocation prefix length
  static constexpr folly::StringPiece kSeedPrefixAllocParamKey{
      "e2e-network-prefix"};

  // kvstore key for prefix allocator parameters indicating static allocation
  static constexpr folly::StringPiece kStaticPrefixAllocParamKey{
      "e2e-network-allocations"};

  //
  // LinkMonitor specific
  //

  // Hold time to wait before advertising link events. We use different
  // timers for UP (Throttle=100ms) and DOWN events are immediately
  static constexpr std::chrono::milliseconds kLinkThrottleTimeout{100};
  static constexpr std::chrono::milliseconds kLinkImmediateTimeout{1};

  // Hold time to wait before advertising adjacency UP event to KvStore.
  // Adjacency DOWN event is immediately advertised.
  static constexpr std::chrono::milliseconds kAdjacencyThrottleTimeout{1000};

  //
  // Spark specific
  //

  // the multicast address used by Spark
  static constexpr folly::StringPiece kSparkMcastAddr{"ff02::1"};

  // The maximum number of spark packets per second we will process from
  // a iface, ip addr pairs that hash to the same bucket in our
  // fixed size list of BucketedTimeSeries
  static constexpr uint32_t kMaxAllowedPps{50};

  // Number of BucketedTimeSeries to spread potential neighbors across
  // for the purpose of limiting the number of packets per second processed
  static constexpr size_t kNumTimeSeries{1024};

  //
  // Platform/Fib specific
  //

  // Default const parameters for thrift connections with Switch agent
  static constexpr folly::StringPiece kPlatformHost{"::1"};
  static constexpr std::chrono::milliseconds kPlatformConnTimeout{100};
  static constexpr std::chrono::milliseconds kPlatformRoutesProcTimeout{20000};
  static constexpr std::chrono::milliseconds kServiceConnTimeout{500};
  static constexpr std::chrono::milliseconds kServiceConnSSLTimeout{1000};
  static constexpr std::chrono::milliseconds kServiceProcTimeout{2500};

  // time interval to sync between Open/R and Platform
  static constexpr std::chrono::seconds kPlatformSyncInterval{60};

  // time interval for keep alive check between fib and switch agent
  static constexpr std::chrono::milliseconds kKeepAliveCheckInterval{1000};

  // Timeout duration for which if a client connection has no activity, then it
  // will be dropped. We keep it 3 * kPlatformSyncInterval so that thrift
  // connection between OpenR and platform service remains up forever under
  // ideal conditions.
  static constexpr std::chrono::seconds kPlatformThriftIdleTimeout{
      Constants::kPlatformSyncInterval * 3};

  // PrefixAllocator address programming retry interval 100 ms
  static constexpr std::chrono::milliseconds kPrefixAllocatorRetryInterval{100};

  //
  // KvStore specific
  //

  // the time we hold on to clear keys from KvStore
  static constexpr std::chrono::milliseconds kKvStoreClearThrottleTimeout{10};

  // the time we hold on to announce to KvStore
  static constexpr std::chrono::milliseconds kKvStoreSyncThrottleTimeout{100};

  // Kvstore timer for flooding pending publication
  static constexpr std::chrono::milliseconds kFloodPendingPublication{100};

  // KvStore database TTLs
  static constexpr std::chrono::milliseconds kKvStoreDbTtl{5min};

  // RangeAllocator keys TTLs
  static constexpr std::chrono::milliseconds kRangeAllocTtl{5min};

  // delimiter separating prefix and name in kvstore key
  static constexpr folly::StringPiece kPrefixNameSeparator{":"};

  // KvStore default areaId
  //
  // NOTE: this is a special areaId that is treated as the wildcard area.
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
  // int version
  static constexpr int64_t kTtlInfinity{INT32_MIN};
  // ttl to decrement before re-flooding
  static constexpr std::chrono::milliseconds kTtlDecrement{1};
  // min ttl expiry time to qualify for peer sync
  static constexpr std::chrono::milliseconds kTtlThreshold{500};
  // ms version
  static constexpr std::chrono::milliseconds kTtlInfInterval{kTtlInfinity};

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
