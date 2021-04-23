/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Constants.h"

namespace openr {

constexpr double Constants::kRttChangeThreashold;
constexpr folly::StringPiece Constants::kAdjDbMarker;
constexpr folly::StringPiece Constants::kErrorResponse;
constexpr folly::StringPiece Constants::kEventLogCategory;
constexpr folly::StringPiece Constants::kFibTimeMarker;
constexpr folly::StringPiece Constants::kGlobalCmdLocalIdTemplate;
constexpr folly::StringPiece Constants::kNodeLabelRangePrefix;
constexpr folly::StringPiece Constants::kOpenrCtrlSessionContext;
constexpr folly::StringPiece Constants::kPlatformHost;
constexpr folly::StringPiece Constants::kPrefixAllocMarker;
constexpr folly::StringPiece Constants::kPrefixDbMarker;
constexpr folly::StringPiece Constants::kPrefixNameSeparator;
constexpr folly::StringPiece Constants::kSeedPrefixAllocLenSeparator;
constexpr folly::StringPiece Constants::kSeedPrefixAllocParamKey;
constexpr folly::StringPiece Constants::kSparkMcastAddr;
constexpr folly::StringPiece Constants::kStaticPrefixAllocParamKey;
constexpr folly::StringPiece Constants::kSuccessResponse;
constexpr int Constants::kHighWaterMark;
constexpr int Constants::kIpTos;
constexpr int Constants::kKeepAliveCnt;
constexpr int Constants::kKeepAliveEnable;
constexpr int32_t Constants::kDefaultPathPreference;
constexpr int32_t Constants::kDefaultSourcePreference;
constexpr int32_t Constants::kFibAgentPort;
constexpr int32_t Constants::kKvStoreRepPort;
constexpr int32_t Constants::kOpenrCtrlPort;
constexpr int32_t Constants::kSparkMcastPort;
constexpr int32_t Constants::kMaxSrLabel;
constexpr int32_t Constants::kOpenrSupportedVersion;
constexpr int32_t Constants::kOpenrVersion;
constexpr int64_t Constants::kDefaultAdjWeight;
constexpr int64_t Constants::kTtlInfinity;
constexpr size_t Constants::kMaxFullSyncPendingCountThreshold;
constexpr size_t Constants::kNumTimeSeries;
constexpr std::chrono::milliseconds Constants::kAdjacencyThrottleTimeout;
constexpr std::chrono::milliseconds Constants::kFibInitialBackoff;
constexpr std::chrono::milliseconds Constants::kFibMaxBackoff;
constexpr std::chrono::milliseconds Constants::kFloodPendingPublication;
constexpr std::chrono::milliseconds Constants::kInitialBackoff;
constexpr std::chrono::milliseconds Constants::kKeepAliveCheckInterval;
constexpr std::chrono::milliseconds Constants::kKvStoreDbTtl;
constexpr std::chrono::milliseconds Constants::kKvStoreClearThrottleTimeout;
constexpr std::chrono::milliseconds Constants::kKvStoreSyncThrottleTimeout;
constexpr std::chrono::milliseconds Constants::kLinkImmediateTimeout;
constexpr std::chrono::milliseconds Constants::kLinkThrottleTimeout;
constexpr std::chrono::milliseconds Constants::kLongPollReqHoldTime;
constexpr std::chrono::milliseconds Constants::kMaxBackoff;
constexpr std::chrono::milliseconds Constants::kMaxTtlUpdateInterval;
constexpr std::chrono::milliseconds Constants::kPersistentStoreInitialBackoff;
constexpr std::chrono::milliseconds Constants::kPersistentStoreMaxBackoff;
constexpr std::chrono::milliseconds Constants::kPlatformConnTimeout;
constexpr std::chrono::milliseconds Constants::kPlatformIntfProcTimeout;
constexpr std::chrono::milliseconds Constants::kPlatformRoutesProcTimeout;
constexpr std::chrono::milliseconds Constants::kPollTimeout;
constexpr std::chrono::milliseconds Constants::kPrefixAllocatorRetryInterval;
constexpr std::chrono::milliseconds Constants::kPrefixAllocatorSyncInterval;
constexpr std::chrono::milliseconds Constants::kRangeAllocTtl;
constexpr std::chrono::milliseconds Constants::kReadTimeout;
constexpr std::chrono::milliseconds Constants::kServiceConnTimeout;
constexpr std::chrono::milliseconds Constants::kServiceConnSSLTimeout;
constexpr std::chrono::milliseconds Constants::kServiceProcTimeout;
constexpr std::chrono::milliseconds Constants::kTtlDecrement;
constexpr std::chrono::milliseconds Constants::kTtlInfInterval;
constexpr std::chrono::milliseconds Constants::kTtlThreshold;
constexpr std::chrono::seconds Constants::kConvergenceMaxDuration;
constexpr std::chrono::seconds Constants::kCounterSubmitInterval;
constexpr std::chrono::seconds Constants::kKeepAliveIntvl;
constexpr std::chrono::seconds Constants::kKeepAliveTime;
constexpr std::chrono::seconds Constants::kMemoryThresholdTime;
constexpr std::chrono::seconds Constants::kNetlinkSyncThrottleInterval;
constexpr std::chrono::seconds Constants::kPlatformSyncInterval;
constexpr std::chrono::seconds Constants::kPlatformThriftIdleTimeout;
constexpr std::chrono::seconds Constants::kStoreSyncInterval;
constexpr std::chrono::seconds Constants::kThriftClientKeepAliveInterval;
constexpr std::pair<int32_t, int32_t> Constants::kSrGlobalRange;
constexpr std::pair<int32_t, int32_t> Constants::kSrLocalRange;
constexpr uint16_t Constants::kPerfBufferSize;
constexpr uint32_t Constants::kMaxAllowedPps;
constexpr uint64_t Constants::kOverloadNodeMetric;
constexpr uint8_t Constants::kAqRouteProtoId;

} // namespace openr
