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

class Constants {
 public:
  //
  // Common
  //

  // the string we expect as an error response to a query
  static const std::string kErrorResponse;

  // the string we expect in reply to a successful request
  static const std::string kSuccessResponse;

  // this is used to periodically break from the poll waiting
  // and perform other functions
  static const std::chrono::milliseconds kPollTimeout;

  // this is the maximum time we wait for read data on a socket
  // this is an important constant, as we do not implement any
  // recovery from read errors. We expect in our network reads
  // to be "fast" since we talk to directly adjacent nodes
  static const std::chrono::milliseconds kReadTimeout;

  // Default keepAlive values
  static const int kKeepAliveEnable;
  /* Idle Time before sending keep alives */
  static const std::chrono::seconds kKeepAliveTime;
  /* max keep alives before resetting connection */
  static const int kKeepAliveCnt;
  /* interval between keep alives */
  static const std::chrono::seconds kKeepAliveIntvl;

  // The maximum messages we can queue on sending socket
  static const int kHighWaterMark;

  // Maximum label size
  static const int32_t kMaxSrLabel;

  // Segment Routing namespace constants. Local and Global ranges are exclusive
  static const std::pair<int32_t /* low */, int32_t /* high */> kSrGlobalRange;
  static const std::pair<int32_t /* low */, int32_t /* high */> kSrLocalRange;

  // IP TOS to be used for all control IP packets in network flowing across
  // the nodes
  static const int kIpTos;

  // default interval to publish to monitor
  static const std::chrono::seconds kMonitorSubmitInterval;

  // event log category
  static const std::string kEventLogCategory;

  // ExponentialBackoff durations
  static const std::chrono::milliseconds kInitialBackoff;
  static const std::chrono::milliseconds kMaxBackoff;

  //
  // KvStore specific

  // default interval for kvstore to sync with peers
  static const std::chrono::seconds kStoreSyncInterval;

  //
  // PrefixAllocator specific

  // default interval for prefix allocator to sync with kvstore
  static const std::chrono::milliseconds kPrefixAllocatorSyncInterval;

  // seed prefix and allocated prefix length is separate by comma
  static const std::string kSeedPrefixAllocLenSeparator;

  // kvstore key for prefix allocator parameters indicating seed prefix and
  // allocation prefix length
  static const std::string kSeedPrefixAllocParamKey;

  // kvstore key for prefix allocator parameters indicating static allocation
  static const std::string kStaticPrefixAllocParamKey;

  //
  // LinkMonitor specific
  //

  // the time we hold on announcing a link when it comes up
  static const std::chrono::milliseconds kLinkThrottleTimeout;

  // overloaded note metric value
  static const uint64_t kOverloadNodeMetric;

  //
  // Spark specific
  //

  // the maximun keep-alive interval for spark messages
  static const std::chrono::seconds kMaxKeepAliveInterval;

  // how many times to retry send or receive before failing
  static const uint32_t kNumRecvSendRetries;

  // the multicast address used by Spark
  static const folly::IPAddress kSparkMcastAddr;

  // Required percentage change in measured RTT for announcing new RTT
  static const double kRttChangeThreashold;

  // The maximum number of spark packets per second we will process from
  // a iface, ip addr pairs that hash to the same bucket in our
  // fixed size list of BucketedTimeSeries
  static const uint32_t kMaxAllowedPps;

  // Number of BucketedTimeSeries to spread potential neighbors across
  // for the prurpose of limiting the number of packets per second processed
  static const size_t kNumTimeSeries;

  //
  // Platform/Fib specific
  //

  // Default const parameters for thrift connections with Switch agent
  static const std::string kPlatformHost;
  static const std::chrono::milliseconds kPlatformConnTimeout;
  static const std::chrono::milliseconds kPlatformProcTimeout;

  // Protocol ID for OpenR routes
  static const uint8_t kAqRouteProtoId;

  //
  // HealthChecker specific
  //

  // time interval between sending two health msg
  static const std::chrono::milliseconds kHealthCheckInterval;

  // time interval to sync between fib and agent
  static const std::chrono::seconds kSyncFibInterval;

  //
  // KvStore specific
  //

  // KvStore database TTLs
  static const std::chrono::milliseconds kKvStoreDbTtl;

  // RangeAllocator keys TTLs
  static const std::chrono::milliseconds kRangeAllocTtl;

  // delimiter separating prefix and name in kvstore key
  static const std::string kPrefixNameSeparator;

  // KvStore key markers
  static const std::string kAdjDbMarker;
  static const std::string kInterfaceDbMarker;
  static const std::string kPrefixDbMarker;
  static const std::string kPrefixAllocMarker;
  static const std::string kNodeLabelRangePrefix;

  // ID template for local command socket
  static const std::string kLocalCmdIdTemplate;
  // ID template for global command socket
  static const std::string kGlobalCmdIdTemplate;
  // ID template for peer sync socket
  static const std::string kPeerSyncIdTemplate;
  // ID template for the global PUB socket
  static const std::string kGlobalPubIdTemplate;
  // ID template for the global SUB socket
  static const std::string kGlobalSubIdTemplate;

  // max interval to update TTL for each key in kvstore w/ finite TTL
  static const std::chrono::milliseconds kMaxTtlUpdateInterval;
  // TTL infinity, never expires
  // int version
  static const int64_t kTtlInfinity;
  // ms version
  static const std::chrono::milliseconds kTtlInfInterval;

  // adjacencies can have weights for weighted ecmp
  static const int64_t kDefaultAdjWeight;

  // buffer size to keep latest perf log
  static const uint16_t kPerfBufferSize;
  static const std::chrono::seconds kConvergenceMaxDuration;

  // OpenR ports

  // KvStore publisher port for emitting realtime key-value deltas
  static const int32_t kKvStorePubPort;

  // The port KvStore replier listens on
  static const int32_t kKvStoreRepPort;

  // Decision publisher port for emitting realtime route-db updates
  static const int32_t kDecisionPubPort;

  // The port Decision replier listens on
  static const int32_t kDecisionRepPort;

  // The port link monitor publishes on
  static const int32_t kLinkMonitorPubPort;

  // The port link monitor listens for commands on
  static const int32_t kLinkMonitorCmdPort;

  // The port monitor publishes on
  static const int32_t kMonitorPubPort;

  // The port monitor replies on
  static const int32_t kMonitorRepPort;

  // The port fib replier listens on
  static const int32_t kFibRepPort;

  // The port health checker sends and recvs udp pings on
  static const int32_t kHealthCheckerPort;

  // The port prefix manager receives commands on
  static const int32_t kPrefixManagerCmdPort;

  // The port Health Checker replier listens on
  static const int32_t kHealthCheckerRepPort;

  // Switch agent thrift service port for Platform programming
  static const int32_t kSystemAgentPort;

  // Switch agent thrift service port for FIB programming
  static const int32_t kFibAgentPort;

  // Spark UDP multicast port for sending spark-hello messages
  static const int32_t kSparkMcastPort;

  // Current OpenR version
  static const int32_t kOpenrVersion;

  // Lowest Supported OpenR version
  static const int32_t kOpenrSupportedVersion;

};

} // namespace openr
