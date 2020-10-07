/**
 * Copyright (c) 2014-present, Facebook, Inc.
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
  //
  // Common
  //

  // the string we expect as an error response to a query
  static constexpr folly::StringPiece kErrorResponse{"ERR"};

  // the string we expect in reply to a successful request
  static constexpr folly::StringPiece kSuccessResponse{"OK"};

  // this is used to periodically break from the poll waiting
  // and perform other functions
  static constexpr std::chrono::milliseconds kPollTimeout{50};

  // this is the maximum time we wait for read data on a socket
  // this is an important constant, as we do not implement any
  // recovery from read errors. We expect in our network reads
  // to be "fast" since we talk to directly adjacent nodes
  static constexpr std::chrono::milliseconds kReadTimeout{1000};

  // Default keepAlive values
  static constexpr int kKeepAliveEnable{1};
  // Idle Time before sending keep alives
  static constexpr std::chrono::seconds kKeepAliveTime{30};
  // max keep alives before resetting connection
  static constexpr int kKeepAliveCnt{6};
  // interval between keep alives
  static constexpr std::chrono::seconds kKeepAliveIntvl{5};

  // The maximum messages we can queue on sending socket
  static constexpr int kHighWaterMark{65536};

  // Maximum label size
  static constexpr int32_t kMaxSrLabel{(1 << 20) - 1};

  // Segment Routing namespace constants. Local and Global ranges are exclusive
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrGlobalRange{101, 49999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrLocalRange{50000, 59999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
      kSrStaticMplsRouteRange{60000, 69999};

  // IP TOS to be used for all control IP packets in network flowing across
  // the nodes
  // DSCP = 48 (first 6 bits), ECN = 0 (last 2 bits). Total 192
  static constexpr int kIpTos{0x30 << 2};

  // default interval to publish to monitor
  static constexpr std::chrono::seconds kCounterSubmitInterval{5};

  // event log category
  static constexpr folly::StringPiece kEventLogCategory{"perfpipe_aquaman"};

  // ExponentialBackoff durations
  // Link-monitor, KvStore
  static constexpr std::chrono::milliseconds kInitialBackoff{64};
  static constexpr std::chrono::milliseconds kMaxBackoff{8192};
  // FIB Sync, perhaps this could be removed and above used in time.
  static constexpr std::chrono::milliseconds kFibSyncInitialBackoff{8};
  static constexpr std::chrono::milliseconds kFibSyncMaxBackoff{4096};

  // Persistent store specific
  static constexpr std::chrono::milliseconds kPersistentStoreInitialBackoff{
      100};
  static constexpr std::chrono::milliseconds kPersistentStoreMaxBackoff{5000};

  //
  // KvStore specific

  // default interval for kvstore to sync with peers
  static constexpr std::chrono::seconds kStoreSyncInterval{60};

  // default thrift client keep alive interval to avoid idle timeout
  static constexpr std::chrono::seconds kThriftClientKeepAliveInterval{30};

  // Count of maximum pending kvstore sync response before waiting for
  // kMaxBackoff to send the next sync request
  static constexpr size_t kMaxFullSyncPendingCountThreshold{32};

  //
  // PrefixAllocator specific

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

  // the time we hold on announcing a link when it comes up
  static constexpr std::chrono::milliseconds kLinkThrottleTimeout{1000};
  static constexpr std::chrono::milliseconds kLinkImmediateTimeout{1};

  // overloaded note metric value
  static constexpr uint64_t kOverloadNodeMetric{1ull << 32};

  //
  // Spark specific
  //

  // the multicast address used by Spark
  static constexpr folly::StringPiece kSparkMcastAddr{"ff02::1"};

  // Required percentage change in measured RTT for announcing new RTT
  static constexpr double kRttChangeThreashold{10.0};

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
  static constexpr std::chrono::milliseconds kPlatformIntfProcTimeout{1000};
  static constexpr std::chrono::milliseconds kServiceConnTimeout{500};
  static constexpr std::chrono::milliseconds kServiceProcTimeout{20000};

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

  // Duration for throttling full sync of network state from kernel via netlink
  static constexpr std::chrono::seconds kNetlinkSyncThrottleInterval{3};

  // PrefixAllocator address programming retry interval 100 ms
  static constexpr std::chrono::milliseconds kPrefixAllocatorRetryInterval{100};

  // Protocol ID for OpenR routes
  static constexpr uint8_t kAqRouteProtoId{99};

  //
  // KvStore specific
  //

  // Kvstore timer for flooding pending publication
  static constexpr std::chrono::milliseconds kFloodPendingPublication{100};

  // KvStore database TTLs
  static constexpr std::chrono::milliseconds kKvStoreDbTtl{5min};

  // RangeAllocator keys TTLs
  static constexpr std::chrono::milliseconds kRangeAllocTtl{5min};

  // delimiter separating prefix and name in kvstore key
  static constexpr folly::StringPiece kPrefixNameSeparator{":"};

  // KvStore key markers
  static constexpr folly::StringPiece kAdjDbMarker{"adj:"};
  static constexpr folly::StringPiece kPrefixDbMarker{"prefix:"};
  static constexpr folly::StringPiece kPrefixAllocMarker{"allocprefix:"};
  static constexpr folly::StringPiece kFibTimeMarker{"fibtime:"};
  static constexpr folly::StringPiece kNodeLabelRangePrefix{"nodeLabel:"};

  static constexpr folly::StringPiece kGlobalCmdLocalIdTemplate{
      "{}::{}::TCP::CMD::LOCAL"};

  static constexpr folly::StringPiece kOpenrCtrlSessionContext{"OpenrCtrl"};
  static constexpr folly::StringPiece kPluginSessionContext{"OpenrPlugin"};

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

  // the time we hold on to announce to KvStore
  static constexpr std::chrono::milliseconds kPrefixMgrKvThrottleTimeout{250};

  // Default metrics (path and source preference) for Open/R originated routes
  // (loopback address & interface subnets).
  static constexpr int32_t kDefaultPathPreference{1000}; // LIVE routes
  static constexpr int32_t kDefaultSourcePreference{200}; // Source pref

  // OpenR ports

  // Openr Ctrl thrift server port
  static constexpr int32_t kOpenrCtrlPort{2018};

  // The port KvStore replier listens on
  static constexpr int32_t kKvStoreRepPort{60002};

  // Switch agent thrift service port for FIB programming
  static constexpr int32_t kFibAgentPort{60100};

  // Spark UDP multicast port for sending spark-hello messages
  static constexpr int32_t kSparkMcastPort{6666};

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
