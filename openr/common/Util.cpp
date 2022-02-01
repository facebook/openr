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

// construct thrift::KvStoreFloodRate
thrift::KvStoreFloodRate
createKvStoreFloodRate(
    int32_t flood_msg_per_sec, int32_t flood_msg_burst_size) {
  thrift::KvStoreFloodRate floodRate;
  floodRate.flood_msg_per_sec_ref() = flood_msg_per_sec;
  floodRate.flood_msg_burst_size_ref() = flood_msg_burst_size;
  return floodRate;
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
