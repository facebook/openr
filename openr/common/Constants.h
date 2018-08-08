/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
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
    kSrGlobalRange{1, 49999};
  static constexpr std::pair<int32_t /* low */, int32_t /* high */>
    kSrLocalRange{50000, 59999};

  // IP TOS to be used for all control IP packets in network flowing across
  // the nodes
  // DSCP = 48 (first 6 bits), ECN = 0 (last 2 bits). Total 192
  static constexpr int kIpTos{0x30 << 2};

  // default interval to publish to monitor
  static constexpr std::chrono::seconds kMonitorSubmitInterval{5};

  // event log category
  static constexpr folly::StringPiece kEventLogCategory{"perfpipe_aquaman"};

  // ExponentialBackoff durations
  static constexpr std::chrono::milliseconds kInitialBackoff{64};
  static constexpr std::chrono::milliseconds kMaxBackoff{8192};

  //
  // KvStore specific

  // default interval for kvstore to sync with peers
  static constexpr std::chrono::seconds kStoreSyncInterval{60};

  //
  // PrefixAllocator specific

  // default interval for prefix allocator to sync with kvstore
  static constexpr std::chrono::milliseconds kPrefixAllocatorSyncInterval{1000};

  // seed prefix and allocated prefix length is separate by comma
  static constexpr folly::StringPiece kSeedPrefixAllocLenSeparator{","};

  // kvstore key for prefix allocator parameters indicating seed prefix and
  // allocation prefix length
  static constexpr folly::StringPiece kSeedPrefixAllocParamKey{
    "e2e-network-prefix"
  };

  // kvstore key for prefix allocator parameters indicating static allocation
  static constexpr folly::StringPiece kStaticPrefixAllocParamKey{
    "e2e-network-allocations"
  };

  //
  // LinkMonitor specific
  //

  // the time we hold on announcing a link when it comes up
  static constexpr std::chrono::milliseconds kLinkThrottleTimeout{1000};

  // overloaded note metric value
  static constexpr uint64_t kOverloadNodeMetric{1ull << 32};

  //
  // Spark specific
  //

  // the maximun keep-alive interval for spark messages
  static constexpr std::chrono::seconds kMaxKeepAliveInterval{3};

  // how many times to retry send or receive before failing
  static constexpr uint32_t kNumRecvSendRetries{3};

  // the multicast address used by Spark
  static constexpr folly::StringPiece kSparkMcastAddr{"ff02::1"};

  // Required percentage change in measured RTT for announcing new RTT
  static constexpr double kRttChangeThreashold{10.0};

  // The maximum number of spark packets per second we will process from
  // a iface, ip addr pairs that hash to the same bucket in our
  // fixed size list of BucketedTimeSeries
  static constexpr uint32_t kMaxAllowedPps{50};

  // Number of BucketedTimeSeries to spread potential neighbors across
  // for the prurpose of limiting the number of packets per second processed
  static constexpr size_t kNumTimeSeries{1024};

  //
  // Platform/Fib specific
  //

  // Default const parameters for thrift connections with Switch agent
  static constexpr folly::StringPiece kPlatformHost{"::1"};
  static constexpr std::chrono::milliseconds kPlatformConnTimeout{100};
  static constexpr std::chrono::milliseconds kPlatformProcTimeout{20000};

  // time interval to sync between Open/R and Platform
  static constexpr std::chrono::seconds kPlatformSyncInterval{60};

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
  // HealthChecker specific
  //

  // time interval between sending two health msg
  static constexpr std::chrono::milliseconds kHealthCheckInterval{1000};

  //
  // KvStore specific
  //

  // KvStore database TTLs
  static constexpr std::chrono::milliseconds kKvStoreDbTtl{5min};

  // RangeAllocator keys TTLs
  static constexpr std::chrono::milliseconds kRangeAllocTtl{5min};

  // delimiter separating prefix and name in kvstore key
  static constexpr folly::StringPiece kPrefixNameSeparator{":"};

  // KvStore key markers
  static constexpr folly::StringPiece kAdjDbMarker{"adj:"};
  static constexpr folly::StringPiece kInterfaceDbMarker{"intf:"};
  static constexpr folly::StringPiece kPrefixDbMarker{"prefix:"};
  static constexpr folly::StringPiece kPrefixAllocMarker{"allocprefix:"};
  static constexpr folly::StringPiece kNodeLabelRangePrefix{"nodeLabel:"};

  // ID template for local command socket
  static constexpr folly::StringPiece kLocalCmdIdTemplate{"{}::IPC::CMD"};
  // ID template for global command socket
  static constexpr folly::StringPiece kGlobalCmdIdTemplate{"{}::TCP::CMD"};
  static constexpr folly::StringPiece kGlobalCmdLocalIdTemplate{
      "{}::{}::TCP::CMD::LOCAL"};
  // ID template for peer sync socket
  static constexpr folly::StringPiece kPeerSyncIdTemplate{"{}::TCP::SYNC"};
  // ID template for the global PUB socket
  static constexpr folly::StringPiece kGlobalPubIdTemplate{"{}::TCP::PUB"};
  // ID template for the global SUB socket
  static constexpr folly::StringPiece kGlobalSubIdTemplate{"{}::TCP::SUB"};

  // max interval to update TTL for each key in kvstore w/ finite TTL
  static constexpr std::chrono::milliseconds kMaxTtlUpdateInterval{2h};
  // TTL infinity, never expires
  // int version
  static constexpr int64_t kTtlInfinity{INT32_MIN};
  // min ttl expiry time to qualify for peer sync
  static constexpr int64_t kTtlThreshold{20};
  // ms version
  static constexpr std::chrono::milliseconds kTtlInfInterval{kTtlInfinity};

  // adjacencies can have weights for weighted ecmp
  static constexpr int64_t kDefaultAdjWeight{1};

  // buffer size to keep latest perf log
  static constexpr uint16_t kPerfBufferSize{10};
  static constexpr std::chrono::seconds kConvergenceMaxDuration{3s};

  // OpenR ports

  // KvStore publisher port for emitting realtime key-value deltas
  static constexpr int32_t kKvStorePubPort{60001};

  // The port KvStore replier listens on
  static constexpr int32_t kKvStoreRepPort{60002};

  // Decision publisher port for emitting realtime route-db updates
  static constexpr int32_t kDecisionPubPort{60003};

  // The port Decision replier listens on
  static constexpr int32_t kDecisionRepPort{60004};

  // The port link monitor publishes on
  static constexpr int32_t kLinkMonitorPubPort{60005};

  // The port link monitor listens for commands on
  static constexpr int32_t kLinkMonitorCmdPort{60006};

  // The port monitor publishes on
  static constexpr int32_t kMonitorPubPort{60007};

  // The port monitor replies on
  static constexpr int32_t kMonitorRepPort{60008};

  // The port fib replier listens on
  static constexpr int32_t kFibRepPort{60009};

  // The port health checker sends and recvs udp pings on
  static constexpr int32_t kHealthCheckerPort{60010};

  // The port prefix manager receives commands on
  static constexpr int32_t kPrefixManagerCmdPort{60011};

  // The port Health Checker replier listens on
  static constexpr int32_t kHealthCheckerRepPort{60012};

  // Switch agent thrift service port for Platform programming
  static constexpr int32_t kSystemAgentPort{60099};

  // Switch agent thrift service port for FIB programming
  static constexpr int32_t kFibAgentPort{60100};

  // Spark UDP multicast port for sending spark-hello messages
  static constexpr int32_t kSparkMcastPort{6666};

  // Current OpenR version
  static constexpr int32_t kOpenrVersion{20180307};

  // Lowest Supported OpenR version
  static constexpr int32_t kOpenrSupportedVersion{20180307};

  // Threshold time in secs to crash after reaching critical memory
  static constexpr std::chrono::seconds kMemoryThresholdTime{600};
};

} // namespace openr
