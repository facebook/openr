/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Constants.h"

namespace openr {

using namespace std::chrono_literals;

// the string we expect as an error response to a query
const std::string Constants::kErrorResponse{"ERR"};

// the string we expect in reply to a successful request
const std::string Constants::kSuccessResponse{"OK"};

// this is used to periodically break from the poll waiting
// and perform other functions
const std::chrono::milliseconds Constants::kPollTimeout{50};

// this is the maximum time we wait for read data on a socket
// this is an important constant, as we do not implement any
// recovery from read errors. We expect in our network reads
// to be "fast" since we talk to directly adjacent nodes
const std::chrono::milliseconds Constants::kReadTimeout{1000};

const std::chrono::milliseconds Constants::kLinkThrottleTimeout{1000};

const std::chrono::seconds Constants::kStoreSyncInterval{60};

const std::chrono::milliseconds Constants::kPrefixAllocatorSyncInterval{1000};
const std::string Constants::kSeedPrefixAllocLenSeparator{","};
const std::string Constants::kSeedPrefixAllocParamKey{"e2e-network-prefix"};
const std::string Constants::kStaticPrefixAllocParamKey{
  "e2e-network-allocations"};

const std::chrono::seconds Constants::kMonitorSubmitInterval{5};

const std::chrono::seconds Constants::kMaxKeepAliveInterval{3};

// how many times to retry send or receive before failing
const uint32_t Constants::kNumRecvSendRetries{3};

// The maximum messages we can queue on sending socket
const int Constants::kHighWaterMark{65536};

// the multicast address used by Spark
const folly::IPAddress Constants::kSparkMcastAddr{"ff02::1"};

// Switch agent host address. Localhost but can be on separate address as well
const std::string Constants::kPlatformHost{"::1"};

// Timeout values for thrift TCP connection with switch agent
const std::chrono::milliseconds Constants::kPlatformConnTimeout{100};
const std::chrono::milliseconds Constants::kPlatformProcTimeout{10000};

// Required percentage change in measured RTT for announcing new RTT
const double Constants::kRttChangeThreashold{10.0};

// delimiter separating prefix and name in kvstore key
const std::string Constants::kPrefixNameSeparator{":"};

// time interval between sending two health msg
const std::chrono::milliseconds Constants::kHealthCheckInterval{1000};

// time interval to sync between fib and agent
const std::chrono::seconds Constants::kSyncFibInterval{60};

// overloaded note metric value. NOTE: this is greater than 32 bit value
const uint64_t Constants::kOverloadNodeMetric{1ull << 32};

// Max SR label value assuming 20 bits for label
const int32_t Constants::kMaxSrLabel{(1 << 20) - 1};

// Segment Routing namespace constants. Local and Global ranges are exclusive
const std::pair<int32_t, int32_t> Constants::kSrGlobalRange{1, 49999};
const std::pair<int32_t, int32_t> Constants::kSrLocalRange{50000, 59999};

// Protocol ID for OpenR routes
const uint8_t Constants::kAqRouteProtoId{99};

// DSCP = 48 (first 6 bits), ECN = 0 (last 2 bits). Total 192
const int Constants::kIpTos{0x30 << 2};

// event log category
const std::string Constants::kEventLogCategory{"perfpipe_aquaman"};

// The maximum number of spark packets per second we will process from
// a iface, ip addr pairs that hash to the same bucket in our
// fixed size list of BucketedTimeSeries
const uint32_t Constants::kMaxAllowedPps{50};

// Number of BucketedTimeSeries to spread potential neighbors across
// for the prurpose of limiting the number of packets per second processed
const size_t Constants::kNumTimeSeries{1024};

const std::chrono::milliseconds Constants::kKvStoreDbTtl{5min};
const std::chrono::milliseconds Constants::kRangeAllocTtl{5min};

// KvStore key markers
const std::string Constants::kAdjDbMarker{"adj:"};
const std::string Constants::kInterfaceDbMarker{"intf:"};
const std::string Constants::kPrefixDbMarker{"prefix:"};
const std::string Constants::kPrefixAllocMarker{"allocprefix:"};
const std::string Constants::kNodeLabelRangePrefix{"nodeLabel:"};

// ExponentialBackoff constants
const std::chrono::milliseconds Constants::kInitialBackoff{64};
const std::chrono::milliseconds Constants::kMaxBackoff{8192};

// Default keepAlive values
// We intend to garbage collect flows after 1 min of inactivity
const int Constants::kKeepAliveEnable{1};
const std::chrono::seconds Constants::kKeepAliveTime{30};
const int Constants::kKeepAliveCnt{6};
const std::chrono::seconds Constants::kKeepAliveIntvl{5};

// ID format strings for KvStore ZMQ-Sockets
const std::string Constants::kLocalCmdIdTemplate{"{}::IPC::CMD"};
const std::string Constants::kGlobalCmdIdTemplate{"{}::TCP::CMD"};
const std::string Constants::kPeerSyncIdTemplate{"{}::TCP::SYNC"};
const std::string Constants::kGlobalPubIdTemplate{"{}::TCP::PUB"};
const std::string Constants::kGlobalSubIdTemplate{"{}::TCP::SUB"};

const std::chrono::milliseconds Constants::kMaxTtlUpdateInterval{2h};
// use 32, not 64, bits since milliseconds is at least 45 bits, but can be less
// than 64
const int64_t Constants::kTtlInfinity{INT32_MIN};
const std::chrono::milliseconds Constants::kTtlInfInterval{
    Constants::kTtlInfinity};

// adjacencies can have weights for weighted ecmp
const int64_t Constants::kDefaultAdjWeight{1};

// buffer size to keep latest perf log
const uint16_t Constants::kPerfBufferSize{10};
const std::chrono::seconds Constants::kConvergenceMaxDuration{3s};

// OpenR ports

// KvStore publisher port for emitting realtime key-value deltas
const int32_t Constants::kKvStorePubPort{60001};

// The port KvStore replier listens on
const int32_t Constants::kKvStoreRepPort{60002};

// Decision publisher port for emitting realtime route-db updates
const int32_t Constants::kDecisionPubPort{60003};

// The port Decision replier listens on
const int32_t Constants::kDecisionRepPort{60004};

// The port link monitor publishes on
const int32_t Constants::kLinkMonitorPubPort{60005};

// The port link monitor listens for commands on
const int32_t Constants::kLinkMonitorCmdPort{60006};

// The port monitor publishes on
const int32_t Constants::kMonitorPubPort{60007};

// The port monitor replies on
const int32_t Constants::kMonitorRepPort{60008};

// The port fib replier listens on
const int32_t Constants::kFibRepPort{60009};

// The port health checker sends and recvs udp pings on
const int32_t Constants::kHealthCheckerPort{60010};

// The port prefix manager receives commands on
const int32_t Constants::kPrefixManagerCmdPort{60011};

// The port Health Checker replier listens on
const int32_t Constants::kHealthCheckerRepPort{60012};

// Switch agent thrift service port for Platform programming
const int32_t Constants::kSystemAgentPort{60099};

// Switch agent thrift service port for FIB programming
const int32_t Constants::kFibAgentPort{60100};

// Spark UDP multicast port for sending spark-hello messages
const int32_t Constants::kSparkMcastPort{6666};

// Current OpenR version, max build number <= 99
const int32_t Constants::kOpenrVersion{20180307};

// Lowest Supported OpenR version, max build number <= 99
const int32_t Constants::kOpenrSupportedVersion{20180307};

} // namespace openr
