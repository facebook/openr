/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <filesystem>
namespace fs = std::filesystem;

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
          Constants::kInitEventCounterFormat,
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
  floodRate.flood_msg_per_sec() = flood_msg_per_sec;
  floodRate.flood_msg_burst_size() = flood_msg_burst_size;
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
  value.version() = version;
  value.originatorId() = originatorId;
  value.value().from_optional(data);
  value.ttl() = ttl;
  value.ttlVersion() = ttlVersion;
  if (hash.has_value()) {
    value.hash().from_optional(hash);
  } else {
    value.hash() = generateHash(version, originatorId, data);
  }

  return value;
}

// Create thrift::Value without setting thrift::Value.value
// Used for monitoring applications.
thrift::Value
createThriftValueWithoutBinaryValue(const thrift::Value& val) {
  thrift::Value updatedVal;
  updatedVal.version() = *val.version();
  updatedVal.originatorId() = *val.originatorId();
  updatedVal.ttl() = *val.ttl();
  updatedVal.ttlVersion() = *val.ttlVersion();
  if (val.hash().has_value()) {
    updatedVal.hash() = *val.hash();
  }
  return updatedVal;
}

thrift::Publication
createThriftPublication(
    const thrift::KeyVals& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds,
    const std::optional<std::vector<std::string>>& keysToUpdate,
    const std::string& area,
    const std::optional<int64_t> timestamp_ms) {
  thrift::Publication pub;
  pub.keyVals() = kv;
  pub.expiredKeys() = expiredKeys;
  pub.nodeIds().from_optional(nodeIds);
  pub.tobeUpdatedKeys().from_optional(keysToUpdate);
  pub.area() = area;
  pub.timestamp_ms().from_optional(timestamp_ms);
  return pub;
}

bool
matchPrefix(const std::string& key, const std::vector<std::string>& filters) {
  for (auto prefix = filters.begin(); prefix != filters.end(); prefix++) {
    if (key.find(*prefix) == 0) {
      return true;
    }
  }
  return false;
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

std::string
toString(const thrift::KeyDumpParams& filter) {
  std::stringstream result;
  result << "originatorIds: ";
  for (auto i : *filter.originatorIds()) {
    result << i << " ";
  }
  result << std::endl;
  result << "ignore ttl: " << *filter.ignoreTtl() << std::endl;
  if (filter.keys().has_value()) {
    result << "keys: ";
    for (auto i : *filter.keys()) {
      result << i << " ";
    }
  }
  if (filter.senderId().has_value()) {
    result << "senderId: " << *filter.senderId();
  }
  return result.str();
}

std::string
toString(const thrift::KeySetParams& param) {
  std::stringstream result;
  for (auto [k, v] : *param.keyVals()) {
    result << "key: " << k;
    result << " version: " << *v.version()
           << " originatorId: " << *v.originatorId() << " ttl: " << *v.ttl()
           << " ttlVersion: " << *v.ttlVersion();
  }
  if (param.senderId().has_value()) {
    result << " senderId: " << *param.senderId();
  }
  return result.str();
}

std::string
toString(const std::vector<std::string>& input) {
  std::stringstream result;
  for (const auto& item : input) {
    result << item;
    result << " ";
  }
  return result.str();
}
} // namespace openr
