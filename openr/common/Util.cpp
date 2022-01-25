/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>

#include <thrift/lib/cpp/util/EnumUtils.h>

namespace openr {

void
logInitializationEvent(
    const std::string& publisher,
    const thrift::InitializationEvent event,
    const std::optional<std::string>& message) {
  // OpenR start time. Initialized first time function is called.
  const static auto kOpenrStartTime = std::chrono::steady_clock::now();
  // Duration in milliseconds since OpenR start.
  auto durationSinceStart =
      std::chrono::ceil<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - kOpenrStartTime)
          .count();
  auto durationStr = durationSinceStart >= 1000
      ? fmt::format("{}s", durationSinceStart * 1.0 / 1000)
      : fmt::format("{}ms", durationSinceStart);

  auto logMsg = fmt::format(
      "[Initialization] event: {}, publisher: {}, durationSinceStart: {}",
      apache::thrift::util::enumNameSafe(event),
      publisher,
      durationStr);
  if (message.has_value()) {
    logMsg = logMsg + fmt::format(", message: {}", message.value());
  }
  XLOG(INFO) << logMsg;

  // Log OpenR initialization event to fb303::fbData.
  facebook::fb303::fbData->setCounter(
      fmt::format(
          Constants::kInitEventCounterFormat.toString(),
          apache::thrift::util::enumNameSafe(event)),
      durationSinceStart);
}

void
setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext) {
  thriftServer.setSSLPolicy(sslPolicy);
  // Allow non-secure clients on localhost (e.g. breeze / fbmeshd)
  thriftServer.setAllowPlaintextOnLoopback(true);
  thriftServer.setSSLConfig(sslContext);
  if (fs::exists(ticketSeedPath)) {
    thriftServer.watchTicketPathForChanges(ticketSeedPath);
  }
  return;
}

// construct thrift::PerfEvent
thrift::PerfEvent
createPerfEvent(std::string nodeName, std::string eventDescr, int64_t unixTs) {
  thrift::PerfEvent perfEvent;
  perfEvent.nodeName_ref() = nodeName;
  perfEvent.eventDescr_ref() = eventDescr;
  perfEvent.unixTs_ref() = unixTs;
  return perfEvent;
}

void
addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept {
  thrift::PerfEvent event =
      createPerfEvent(nodeName, eventDescr, getUnixTimeStampMs());
  perfEvents.events_ref()->emplace_back(std::move(event));
}

std::vector<std::string>
sprintPerfEvents(const thrift::PerfEvents& perfEvents) noexcept {
  const auto& events = *perfEvents.events_ref();
  if (events.empty()) {
    return {};
  }

  std::vector<std::string> eventStrs;
  auto recentTs = *events.front().unixTs_ref();
  for (auto const& event : events) {
    auto durationMs = *event.unixTs_ref() - recentTs;
    recentTs = *event.unixTs_ref();
    eventStrs.emplace_back(fmt::format(
        "node: {}, event: {}, duration: {}ms, unix-timestamp: {}",
        *event.nodeName_ref(),
        *event.eventDescr_ref(),
        durationMs,
        *event.unixTs_ref()));
  }
  return eventStrs;
}

std::chrono::milliseconds
getTotalPerfEventsDuration(const thrift::PerfEvents& perfEvents) noexcept {
  if (perfEvents.events_ref()->empty()) {
    return std::chrono::milliseconds(0);
  }

  auto recentTs = *perfEvents.events_ref()->front().unixTs_ref();
  auto latestTs = *perfEvents.events_ref()->back().unixTs_ref();
  return std::chrono::milliseconds(latestTs - recentTs);
}

folly::Expected<std::chrono::milliseconds, std::string>
getDurationBetweenPerfEvents(
    const thrift::PerfEvents& perfEvents,
    const std::string& firstName,
    const std::string& secondName) noexcept {
  auto search = std::find_if(
      perfEvents.events_ref()->cbegin(),
      perfEvents.events_ref()->cend(),
      [&firstName](const thrift::PerfEvent& event) {
        return *event.eventDescr_ref() == firstName;
      });
  if (search == perfEvents.events_ref()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find first event: {}", firstName));
  }
  int64_t first = *search->unixTs_ref();
  search = std::find_if(
      search + 1,
      perfEvents.events_ref()->cend(),
      [&secondName](const thrift::PerfEvent& event) {
        return *event.eventDescr_ref() == secondName;
      });
  if (search == perfEvents.events_ref()->cend()) {
    return folly::makeUnexpected(
        fmt::format("Could not find second event: {}", secondName));
  }
  int64_t second = *search->unixTs_ref();
  if (second < first) {
    return folly::makeUnexpected(
        std::string{"Negative duration between first and second event"});
  }
  return std::chrono::milliseconds(second - first);
}

template <class T>
int64_t
generateHashImpl(
    const int64_t version, const std::string& originatorId, const T& value) {
  size_t seed = 0;
  boost::hash_combine(seed, version);
  boost::hash_combine(seed, originatorId);
  if (value.has_value()) {
    boost::hash_combine(seed, value.value());
  }
  return static_cast<int64_t>(seed);
}

int64_t
generateHash(
    const int64_t version,
    const std::string& originatorId,
    const std::optional<std::string>& value) {
  return generateHashImpl(version, originatorId, value);
}

int64_t
generateHash(
    const int64_t version,
    const std::string& originatorId,
    const apache::thrift::optional_field_ref<const std::string&> value) {
  return generateHashImpl(version, originatorId, value);
}

thrift::BuildInfo
getBuildInfoThrift() noexcept {
  thrift::BuildInfo buildInfo;
  buildInfo.buildUser_ref() = BuildInfo::getBuildUser();
  buildInfo.buildTime_ref() = BuildInfo::getBuildTime();
  buildInfo.buildTimeUnix_ref() =
      static_cast<int64_t>(BuildInfo::getBuildTimeUnix());
  buildInfo.buildHost_ref() = BuildInfo::getBuildHost();
  buildInfo.buildPath_ref() = BuildInfo::getBuildPath();
  buildInfo.buildRevision_ref() = BuildInfo::getBuildRevision();
  buildInfo.buildRevisionCommitTimeUnix_ref() =
      static_cast<int64_t>(BuildInfo::getBuildRevisionCommitTimeUnix());
  buildInfo.buildUpstreamRevision_ref() = BuildInfo::getBuildUpstreamRevision();
  buildInfo.buildUpstreamRevisionCommitTimeUnix_ref() =
      BuildInfo::getBuildUpstreamRevisionCommitTimeUnix();
  buildInfo.buildPackageName_ref() = BuildInfo::getBuildPackageName();
  buildInfo.buildPackageVersion_ref() = BuildInfo::getBuildPackageVersion();
  buildInfo.buildPackageRelease_ref() = BuildInfo::getBuildPackageRelease();
  buildInfo.buildPlatform_ref() = BuildInfo::getBuildPlatform();
  buildInfo.buildRule_ref() = BuildInfo::getBuildRule();
  buildInfo.buildType_ref() = BuildInfo::getBuildType();
  buildInfo.buildTool_ref() = BuildInfo::getBuildTool();
  buildInfo.buildMode_ref() = BuildInfo::getBuildMode();
  return buildInfo;
}

// construct thrift::Value
thrift::Value
createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl,
    int64_t ttlVersion,
    std::optional<int64_t> hash) {
  thrift::Value value;
  value.version_ref() = version;
  value.originatorId_ref() = originatorId;
  value.value_ref().from_optional(data);
  value.ttl_ref() = ttl;
  value.ttlVersion_ref() = ttlVersion;
  if (hash.has_value()) {
    value.hash_ref().from_optional(hash);
  } else {
    value.hash_ref() = generateHash(version, originatorId, data);
  }

  return value;
}

// Create thrift::Value without setting thrift::Value.value
// Used for monitoring applications.
thrift::Value
createThriftValueWithoutBinaryValue(const thrift::Value& val) {
  thrift::Value updatedVal;
  updatedVal.version_ref() = *val.version_ref();
  updatedVal.originatorId_ref() = *val.originatorId_ref();
  updatedVal.ttl_ref() = *val.ttl_ref();
  updatedVal.ttlVersion_ref() = *val.ttlVersion_ref();
  if (val.hash_ref().has_value()) {
    updatedVal.hash_ref() = *val.hash_ref();
  }
  return updatedVal;
}

thrift::Publication
createThriftPublication(
    const std::unordered_map<std::string, thrift::Value>& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds,
    const std::optional<std::vector<std::string>>& keysToUpdate,
    const std::optional<std::string>& floodRootId,
    const std::string& area) {
  thrift::Publication pub;
  *pub.keyVals_ref() = kv;
  *pub.expiredKeys_ref() = expiredKeys;
  pub.nodeIds_ref().from_optional(nodeIds);
  pub.tobeUpdatedKeys_ref().from_optional(keysToUpdate);
  pub.floodRootId_ref().from_optional(floodRootId);
  *pub.area_ref() = area;
  return pub;
}

namespace memory {

uint64_t
getThreadBytesImpl(bool isAllocated) {
  uint64_t bytes{0};
  const char* cmd = isAllocated ? "thread.allocated" : "thread.deallocated";
  try {
    folly::mallctlRead(cmd, &bytes);
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to read thread allocated/de-allocated bytes: "
              << ex.what();
  }
  return bytes;
}

} // namespace memory
} // namespace openr
