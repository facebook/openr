/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <regex>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>
#include <fbzmq/service/if/gen-cpp2/Monitor_types.h>
#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/AllocPrefix_types.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/KnownKeys_types.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace std {

/**
 * Make IpPrefix hashable
 */
template <>
struct hash<openr::thrift::IpPrefix> {
  size_t operator()(openr::thrift::IpPrefix const&) const;
};

} // namespace std

using KeyPair = fbzmq::KeyPair;

namespace openr {

/**
 * Utility function to execute shell command and return true/false as indication
 * of it's success
 */
int executeShellCommand(const std::string& command);

// get prefix length from ipv6 mask
int maskToPrefixLen(const struct sockaddr_in6* mask);

// get prefix length from ipv4 mask
int maskToPrefixLen(const struct sockaddr_in* mask);

// report all IPv6 prefixes configured on the interface
std::vector<folly::CIDRNetwork> getIfacePrefixes(std::string ifName);

bool checkIncludeExcludeRegex(
    const std::string& name,
    const std::vector<std::regex>& includeRegexList,
    const std::vector<std::regex>& excludeRegexList);

/**
 * @param prefixIndex subprefix index, starting from 0
 * @param mask apply mask to the IP portion
 * @return n-th subprefix of allocated length in seed prefix
 * note: only handle IPv6 and assume seed prefix comes unmasked
 */
folly::CIDRNetwork getNthPrefix(
    const folly::CIDRNetwork& seedPrefix,
    uint32_t allocPrefixLen,
    uint32_t prefixIndex,
    bool mask);

// load key pair from file
template <typename Serializer>
KeyPair
loadKeyPairFromFile(
    const std::string& keyPairFilePath, const Serializer& serializer) {
  std::string keyPairStr;

  if (!folly::readFile(keyPairFilePath.c_str(), keyPairStr)) {
    throw std::runtime_error(
        folly::sformat("Failed loading key pair file {}", keyPairFilePath));
  }

  try {
    auto thriftKeyPair = fbzmq::util::readThriftObjStr<thrift::CurveKeyPair>(
        keyPairStr, serializer);

    return {thriftKeyPair.privateKey, thriftKeyPair.publicKey};
  } catch (const std::exception& e) {
    LOG(ERROR) << "Could not parse key pair from file " << keyPairFilePath;
    // rethrow
    throw;
  }
}

// save key pair to file
template <typename Serializer>
void
saveKeyPairToFile(
    const std::string& keyPairFilePath,
    const KeyPair& keyPair,
    const Serializer& serializer) {
  std::string keyPairStr;

  thrift::CurveKeyPair thriftKeyPair(
      apache::thrift::FRAGILE, keyPair.privateKey, keyPair.publicKey);

  try {
    keyPairStr = fbzmq::util::writeThriftObjStr(thriftKeyPair, serializer);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Could not serialize key pair";
    // rethrow
    throw;
  }

  if (!folly::writeFile(keyPairStr, keyPairFilePath.c_str())) {
    throw std::runtime_error(
        folly::sformat("Failed saving key pair to file {}", keyPairFilePath));
  }
}

/**
 * API to flush addresses on the interface. It will flush all addresses
 * under given subnet.
 * If `flushAllGlobalAddrs` is set to true then all global addresses on the
 * interface will be flushed away (for same protocol version as of prefix).
 */
bool flushIfaceAddrs(
    const std::string& ifName,
    const folly::CIDRNetwork& prefix,
    bool flushAllGlobalAddrs);

/**
 * API to add address on the interface.
 * @return boolean indicating status of addr-add operation
 */
bool addIfaceAddr(const std::string& ifName, const folly::IPAddress& addr);

/**
 * Helper function to create loopback address (/128) out of network block.
 * Ideally any address in the block is valid address, in this case we just set
 * last bit of network block to `1`
 */
folly::IPAddress createLoopbackAddr(const folly::CIDRNetwork& prefix) noexcept;

std::unordered_map<std::string, fbzmq::thrift::Counter> prepareSubmitCounters(
    const std::unordered_map<std::string, int64_t>& counters);

template <typename SetT, typename T = typename SetT::value_type>
SetT
buildSetDifference(SetT const& lhs, SetT const& rhs) {
  std::vector<T> result;
  std::copy_if(
      lhs.begin(), lhs.end(), std::back_inserter(result), [&rhs](T const& val) {
        return rhs.find(val) == rhs.end();
      });

  SetT diff(
      std::make_move_iterator(result.begin()),
      std::make_move_iterator(result.end()));

  return diff;
}

inline int64_t
getUnixTimeStamp() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/**
 * Add a perf event
 */
void addPerfEvent(
    thrift::PerfEvents& perfEvents,
    const std::string& nodeName,
    const std::string& eventDescr) noexcept;

// util for parsing lists from the command line
std::vector<std::string> splitByComma(const std::string& input);

/**
 * Print perf event and return total convergence time
 */
std::vector<std::string> sprintPerfEvents(
    const thrift::PerfEvents& perfEvents) noexcept;
std::chrono::milliseconds getTotalPerfEventsDuration(
    const thrift::PerfEvents& perfEvents) noexcept;

/**
 * Generate hash for each keyval pair
 * as a abstract of version number, originator and values
 */
int64_t generateHash(
    const int64_t version,
    const std::string& originatorId,
    const folly::Optional<std::string>& value);

/**
 * TO BE DEPRECATED SOON: Backward compatible with empty remoteIfName
 * Translate remote interface name from local interface name
 * This is only applicable when remoteIfName is empty from peer adjacency update
 * It returns remoteIfName if it is there else constructs one from localIfName
 */
std::string getRemoteIfName(
  const thrift::Adjacency& adj);
} // namespace openr
