/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <folly/memory/MallctlHelper.h>
#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <wangle/ssl/SSLContextConfig.h>

/**
 * Helper macro function to log execution time of function.
 * Usage:
 *  void someFunction() {
 *    LOG_FN_EXECUTION_TIME;
 *    ...
 *    <function-body>
 *    ...
 *  }
 *
 * Output:
 *  V0402 16:35:54 ... UtilTest.cpp:970] Execution time for TestBody took 0ms.
 */

#define LOG_FN_EXECUTION_TIME                                                  \
  auto FB_ANONYMOUS_VARIABLE(SCOPE_EXIT_STATE) =                               \
      ::folly::detail::ScopeGuardOnExit() +                                    \
      [&,                                                                      \
       fn = __FUNCTION__,                                                      \
       ts = std::chrono::steady_clock::now()]() noexcept {                     \
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( \
            std::chrono::steady_clock::now() - ts);                            \
        VLOG(1) << "Execution time for " << fn << " took " << duration.count() \
                << "ms";                                                       \
      }

const openr::AreaId kTestingAreaName{"test_area_name"};

const std::string kTestingNodeName("test_node");

namespace openr {
/**
 * Log OpenR initialization event and export to fb303::fbData.
 */
void logInitializationEvent(
    const std::string& publisher,
    const thrift::InitializationEvent event,
    const std::optional<std::string>& message = std::nullopt);

/**
 * Setup thrift server for TLS
 */
void setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext);

/**
 * Generate hash for each keyval pair
 * as a abstract of version number, originator and values
 * TODO: Remove the API in favor of other one
 */
int64_t generateHash(
    const int64_t version,
    const std::string& originatorId,
    const std::optional<std::string>& value);

int64_t generateHash(
    const int64_t version,
    const std::string& originatorId,
    const apache::thrift::optional_field_ref<const std::string&> value);

/**
 * Utility functions for creating thrift objects
 */
thrift::KvStoreFloodRate createKvStoreFloodRate(
    int32_t flood_msg_per_sec, int32_t flood_msg_burst_size);

thrift::Value createThriftValue(
    int64_t version,
    std::string originatorId,
    std::optional<std::string> data,
    int64_t ttl = Constants::kTtlInfinity,
    int64_t ttlVersion = 0,
    std::optional<int64_t> hash = std::nullopt);

thrift::Value createThriftValueWithoutBinaryValue(const thrift::Value& val);

thrift::Publication createThriftPublication(
    const thrift::KeyVals& kv,
    const std::vector<std::string>& expiredKeys,
    const std::optional<std::vector<std::string>>& nodeIds = std::nullopt,
    const std::optional<std::vector<std::string>>& keysToUpdate = std::nullopt,
    const std::string& area = kTestingAreaName,
    const std::optional<int64_t> timestamp_ms = std::nullopt);

/**
 * Utility function to check if key from a thrift object matches one of the
 * provided filter
 */
bool matchPrefix(
    const std::string& key, const std::vector<std::string>& filters);

/**
 * Return unix timestamp - Number of milliseconds elapsed since the epoch
 */
inline int64_t
getUnixTimeStampMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/**
 * template method to return jittered time based on:
 *
 * @param: base => base value for random number generation;
 * @param: pct => percentage of the deviation from base value;
 */
template <class T>
T
addJitter(T base, double pct = 20.0) {
  CHECK(pct > 0 && pct <= 100) << "percentage input must between 0 and 100";
  thread_local static std::default_random_engine generator;
  std::uniform_int_distribution<int> distribution(
      pct / -100.0 * base.count(), pct / 100.0 * base.count());
  auto roll = std::bind(distribution, generator);
  return T(base.count() + roll());
}

/**
 * template method to return current timestamp using steady clock
 */
template <class T>
T
getCurrentTime() {
  return std::chrono::duration_cast<T>(
      std::chrono::system_clock::now().time_since_epoch());
}

/**
 * Utility functions for conversion between thrift objects and string/IOBuf
 */

template <typename ThriftType, typename Serializer>
std::string
writeThriftObjStr(ThriftType const& obj, Serializer& serializer) {
  std::string result;
  serializer.serialize(obj, &result);
  return result;
}

template <typename ThriftType, typename Serializer>
ThriftType
readThriftObj(folly::IOBuf& buf, Serializer& serializer) {
  ThriftType obj;
  serializer.deserialize(&buf, obj);
  return obj;
}

template <typename ThriftType, typename Serializer>
ThriftType
readThriftObjStr(const std::string& buf, Serializer& serializer) {
  ThriftType obj;
  serializer.deserialize(buf, obj);
  return obj;
}

namespace memory {
uint64_t getThreadBytesImpl(bool isAllocated);
} // namespace memory

std::string toString(const thrift::KeyDumpParams& filter);

std::string toString(const thrift::KeySetParams& param);

std::string toString(const std::vector<std::string>& input);

} // namespace openr
