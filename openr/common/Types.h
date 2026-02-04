/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <re2/re2.h>
#include <re2/set.h>
#include <variant>

#include <boost/heap/d_ary_heap.hpp>
#include <boost/serialization/strong_typedef.hpp>
#include <folly/container/F14Map.h>

#include <openr/common/Constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

BOOST_STRONG_TYPEDEF(std::string, AreaId);

struct RegexSetException : public std::exception {
 public:
  explicit RegexSetException(const std::string& msg) : msg_(msg) {}

  inline const char*
  what(void) const noexcept override {
    return msg_.c_str();
  }

 private:
  const std::string msg_;
};

struct TtlCountdownQueueEntry {
  std::chrono::steady_clock::time_point expiryTime;
  std::string key;
  int64_t version{0};
  int64_t ttlVersion{0};
  std::string originatorId;

  bool
  operator>(TtlCountdownQueueEntry other) const {
    return expiryTime > other.expiryTime;
  }
};

/**
 * Struct defining the key to ttlCountdownHandleMap_.
 */
struct TtlCountdownHandleKey {
  std::string key;
  std::string originatorId;

  bool
  operator==(TtlCountdownHandleKey const& other) const {
    return (key == other.key) && (originatorId == other.originatorId);
  }
};

using TtlCountdownQueue = boost::heap::d_ary_heap<
    TtlCountdownQueueEntry,
    boost::heap::arity<2>, /* each node will have 2 children */
    boost::heap::mutable_<true>, /* allows us to update entries already existing
                                    in the heap */
    // Always returns smallest first
    boost::heap::compare<std::greater<TtlCountdownQueueEntry>>,
    boost::heap::stable<true>>;

/**
 * Structure defining KvStore peer update event in one area.
 */
struct AreaPeerEvent {
  /**
   * Map from nodeName to peer spec, which is expected to be
   * learnt from Spark neighbor discovery. Information will
   * be used for TCP session establishing between KvStore.
   */
  thrift::PeersMap peersToAdd{};

  /**
   * List of nodeName to delete
   */
  std::vector<std::string> peersToDel{};

  explicit AreaPeerEvent(
      const thrift::PeersMap& peersToAdd,
      const std::vector<std::string>& peersToDel)
      : peersToAdd(peersToAdd), peersToDel(peersToDel) {}
};

/**
 * Define peer update event in all areas.
 */
using PeerEvent = folly::F14FastMap<std::string, AreaPeerEvent>;

/**
 * Advertise the key-value into consumer with specified version.
 */
class SetKeyValueRequest {
 public:
  SetKeyValueRequest(
      const AreaId& area,
      const std::string& key,
      const std::string& value,
      const uint32_t version = 0)
      : area(area), key(key), value(value), version(version) {}

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

  inline uint32_t const&
  getVersion() const {
    return version;
  }

 private:
  /**
   * Area identifier.
   */
  AreaId area;
  /**
   * Key to advertise to the consumer.
   */
  std::string key;
  /**
   * Value to advertise to the consumer.
   */
  std::string value;
  /**
   * Version of the key-value pair advertised. Consumer uses the version to
   * determine which value to keep/advertise in case of conflict.
   *
   * If version is not specified, the one greater than the latest known
   * will be used.
   */
  uint32_t version;
};

/**
 * Authoritative request to set specified key-value into consumer.
 *
 * If someone else advertises the same key we try to win over it
 * by re-advertising this key-value with a higher version.
 */
class PersistKeyValueRequest {
 public:
  PersistKeyValueRequest(
      const AreaId& area, const std::string& key, const std::string& value)
      : area(area), key(key), value(value) {}

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

 private:
  /**
   * Area identifier. By default key is published to default area kvstore
   * instance.
   */
  AreaId area;
  /**
   * Key to advertise to the consumer.
   */
  std::string key;
  /**
   * Value to advertise to the consumer.
   */
  std::string value;
};

/**
 * Request to erase key-value from consumer by setting default value of empty
 * string or value passed by the caller, cancel ttl timers, and advertise
 * with higher version.
 */
class ClearKeyValueRequest {
 public:
  ClearKeyValueRequest(
      const AreaId& area,
      const std::string& key,
      const std::string& value = "",
      const bool setValue = false)
      : area(area), key(key), value(value), setValue(setValue) {
    // Value is required if `setValue` flag is true.
    CHECK(!(setValue && value.empty()))
        << "Must specify value in ClearKeyValueRequest when setValue flag is set to true.";
  }

  inline AreaId const&
  getArea() const {
    return area;
  }

  inline std::string const&
  getKey() const {
    return key;
  }

  inline std::string const&
  getValue() const {
    return value;
  }

  inline bool const&
  getSetValue() const {
    return setValue;
  }

 private:
  /**
   * Area identifier.
   */
  AreaId area;
  /**
   * Key to clear from consumer.
   */
  std::string key;
  /**
   * New value to set to key in consumer.
   */
  std::string value;
  /**
   * Flag to set a new value for given key. Value must be provided.
   */
  bool setValue{false};
};

using KeyValueRequest = std::
    variant<SetKeyValueRequest, PersistKeyValueRequest, ClearKeyValueRequest>;

/**
 * TODO: remove this once openr_intialization is by default enabled
 *
 * Structure defining KvStore peer sync event, published to subscribers.
 * LinkMonitor subscribes this event for signal adjancency UP event propagation
 */
struct KvStoreSyncEvent {
  std::string nodeName;
  std::string area;

  KvStoreSyncEvent(const std::string& nodeName, const std::string& area)
      : nodeName(nodeName), area(area) {}

  inline bool
  operator==(const KvStoreSyncEvent& other) const {
    return (nodeName == other.nodeName) && (area == other.area);
  }
};

/**
 * Structure representing:
 *  1) thrift::Publication;
 *  2) Open/R Initialization event;
 */
using KvStorePublication = std::variant<
    thrift::Publication /* publication reflecting key-val change */,
    thrift::InitializationEvent /* KVSTORE_SYNCED */>;

/**
 * Provides match capability on list of regexes. Will default to prefix match
 * if regex is normal string.
 */
class RegexSet {
 public:
  /**
   * Create regex set from list of regexes
   */
  explicit RegexSet(std::vector<std::string> const& regexOrPrefixList);

  /**
   * Match key with regex set
   */
  bool match(std::string const& key) const;

 private:
  std::unique_ptr<re2::RE2::Set> regexSet_;
};

} // namespace openr

template <>
struct std::hash<openr::TtlCountdownHandleKey> {
  size_t
  operator()(const openr::TtlCountdownHandleKey& handleKey) const {
    return folly::hash::hash_combine(handleKey.key, handleKey.originatorId);
  }
};

template <>
struct std::hash<openr::AreaId> {
  size_t
  operator()(openr::AreaId const& areaId) const {
    return hash<string>()(areaId);
  }
};

// Result for pair-wise comparison
enum class ComparisonResult {
  UNKNOWN = 0, // can happen if value is missing
  FIRST = 1, // First one is better
  SECOND = 2, // Second one is better
  TIED = 3,
};
