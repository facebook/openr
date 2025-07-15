/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Constants.h"

namespace openr {

constexpr std::chrono::milliseconds Constants::kFibMaxBackoff;
constexpr std::chrono::milliseconds Constants::kFloodPendingPublication;
constexpr std::chrono::milliseconds Constants::kInitialBackoff;
constexpr std::chrono::milliseconds Constants::kKeepAliveCheckInterval;
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
constexpr std::chrono::milliseconds Constants::kPlatformProcTimeout;
constexpr std::chrono::milliseconds Constants::kReadTimeout;
constexpr std::chrono::milliseconds Constants::kServiceConnTimeout;
constexpr std::chrono::milliseconds Constants::kServiceConnSSLTimeout;
constexpr std::chrono::milliseconds Constants::kServiceProcTimeout;
constexpr std::chrono::milliseconds Constants::kTtlDecrement;
constexpr std::chrono::milliseconds Constants::kTtlInfInterval;
constexpr std::chrono::milliseconds Constants::kTtlThreshold;
constexpr std::chrono::seconds Constants::kConvergenceMaxDuration;
constexpr std::chrono::seconds Constants::kCounterSubmitInterval;
constexpr std::chrono::seconds Constants::kFloodTopoDumpInterval;
constexpr std::chrono::seconds Constants::kMemoryThresholdTime;
constexpr std::chrono::seconds Constants::kPlatformSyncInterval;
constexpr std::chrono::seconds Constants::kPlatformThriftIdleTimeout;
constexpr std::chrono::seconds Constants::kThriftClientKeepAliveInterval;
constexpr uint16_t Constants::kPerfBufferSize;
constexpr uint32_t Constants::kMaxAllowedPps;

} // namespace openr
