/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>
#include <re2/re2.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStoreUtil.h>

namespace openr {
namespace {

bool
isValidTtlAndLog(
    const std::string& key,
    const thrift::Value& value,
    thrift::KvStoreMergeResult& stats) {
  bool result = detail::isValidTtl(*value.ttl());
  if (!result) {
    XLOG(DBG4) << fmt::format(
        "(mergeKeyValues) key: {} has invalid ttl: {}", key, *value.ttl());

    stats.noMergeKeyVals()->emplace(
        std::make_pair(key, thrift::KvStoreNoMergeReason::INVALID_TTL));
  }
  return result;
}

bool
noNeedUpdateAndLog(
    MergeType mergeType,
    const std::string& key,
    thrift::KvStoreMergeResult& result) {
  if (mergeType == MergeType::NO_UPDATE_NEEDED) {
    XLOG(DBG3) << fmt::format(
        "(mergeKeyValues) no need to update anything for key: '{}'", key);
    result.noMergeKeyVals()->emplace(
        key, thrift::KvStoreNoMergeReason::NO_NEED_TO_UPDATE);
    return true;
  }
  return false;
}

bool
isInconsistentAndLog(
    MergeType mergeType,
    const std::string& key,
    thrift::KvStoreMergeResult& result) {
  if (mergeType == MergeType::RESYNC_NEEDED) {
    XLOG(DBG3) << fmt::format(
        "(mergeKeyValues) Inconsistency detected for key: '{}'", key);
    result.noMergeKeyVals()->emplace(
        key, thrift::KvStoreNoMergeReason::INCONSISTENCY_DETECTED);
    result.inconsistencyDetetectedWithOriginator() = true;
    return true;
  }
  return false;
}

bool
isValidVersionAndLog(
    const int64_t myVersion,
    const std::string& key,
    const thrift::Value& value,
    thrift::KvStoreMergeResult& stats) {
  bool result = detail::isValidVersion(myVersion, value);
  if (!result) {
    XLOG(DBG4) << fmt::format(
        "(mergeKeyValues) key: {} has invalid/old version: {}, local version: {}",
        key,
        *value.version(),
        myVersion);

    stats.noMergeKeyVals()->emplace(
        key, thrift::KvStoreNoMergeReason::OLD_VERSION);
  }
  return result;
}

bool
isKeyMatchAndLog(
    std::optional<KvStoreFilters> const& filters,
    const std::string& key,
    const thrift::Value& value,
    thrift::KvStoreMergeResult& stats) {
  if (filters.has_value() && !filters->keyMatch(key, value)) {
    XLOG(DBG4) << fmt::format(
        "(mergeKeyValues) key: {} does NOT matching the fiter: {}",
        key,
        filters->str());
    stats.noMergeKeyVals()->emplace(
        key, thrift::KvStoreNoMergeReason::NO_MATCHED_KEY);
    return false;
  }
  return true;
}

// Incoming value is only updating ttl.
bool
isTtlUpdate(const thrift::Value& value) {
  return !value.value().has_value();
}

/*
 * Inconsistency is detected with 3 following cases:
 * 1. received ttl update with a missing k-v pair
 * 2. received ttl update with a different version
 * 3. received ttl update with a different originatorId
 *
 * When to trigger:
 * 1. There is inconsistency detected.
 * AND
 * 2. Sender is the originator.
 *
 * How to trigger:
 * Setting `inconsistencyDetetectedWithOriginator` to true
 *
 * What would happen:
 * 1. would trigger peer to transition to IDLE state.
 * 2. Trigger originator to bump its key version if needed.
 *
 * Example:
 * A-B-C.
 *
 * If A is the originator of the key, and C receives an inconsistent update
 * from B for A's inconsistency, C will not ask to resync with B.
 */
bool
isResyncNeeded(
    const std::string& key,
    const thrift::Value& value,
    const thrift::Value* curValue) {
  /*
   * ATTENTION: DO NOT CHANGE THE IF-ELSE CONDITION ORDER SINCE THE
   * TIE-BREAKING PROCEDURE HAS PRIORITY.
   */
  bool inconsistencyDetected{false};

  if (curValue == nullptr) {
    /*
     * Case 1: received ttl update with a missing k-v pair
     */
    XLOG(ERR) << "(mergeKeyValues) Detected ttl update from a non-existing "
              << fmt::format(
                     "key: {}, version: {}, originator: {}, ttlVersion: {}",
                     key,
                     *value.version(),
                     *value.originatorId(),
                     *value.ttlVersion());

    inconsistencyDetected = true;
  } else if (*value.version() != *curValue->version()) {
    /*
     * Case 2: received ttl update with a different version
     */
    XLOG(ERR)
        << "(mergeKeyValues) Detected version inconsistency with ttl update. "
        << fmt::format(
               "key: {}, version: {}, originator: {} with local version: {}",
               key,
               *value.version(),
               *value.originatorId(),
               *curValue->version());

    inconsistencyDetected = true;
  } else if (*value.originatorId() != *curValue->originatorId()) {
    /*
     * Case 3: received ttl update with a different originatorId
     */
    XLOG(ERR)
        << "(mergeKeyValues) Detected originatorId inconsistency with ttl update. "
        << fmt::format(
               "key: {}, version: {}, originator: {} with local originatorId: {}",
               key,
               *value.version(),
               *value.originatorId(),
               *curValue->originatorId());

    inconsistencyDetected = true;
  }
  return inconsistencyDetected;
}

void
updateKvStoreValue(
    thrift::KeyVals& kvStore,
    const std::string& key,
    const thrift::Value& value) {
  auto kvStoreIt = kvStore.find(key);

  FB_LOG_EVERY_MS(INFO, 500) << fmt::format(
      "Updating key: {}, Originator: {}, Version: {}, TtlVersion: {}, Ttl: {}",
      key,
      *value.originatorId(),
      *value.version(),
      *value.version(),
      *value.ttl());

  CHECK(value.value().has_value());
  // grab the new value (this will copy, intended)
  thrift::Value newValue = value;
  if (kvStoreIt == kvStore.end()) {
    // create new entry
    std::tie(kvStoreIt, std::ignore) = kvStore.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(key),
        std::forward_as_tuple(std::move(newValue)));
  } else {
    // update the entry in place, the old value will be destructed
    kvStoreIt->second = std::move(newValue);
  }
  // update hash if it's not there
  if (!kvStoreIt->second.hash().has_value()) {
    kvStoreIt->second.hash() =
        generateHash(*value.version(), *value.originatorId(), value.value());
  }
}

// update TTL only, nothing else
void
updateKvStoreTtl(
    thrift::KeyVals& kvStore,
    const std::string& key,
    const thrift::Value& value) {
  auto kvStoreIt = kvStore.find(key);
  CHECK(kvStoreIt != kvStore.end());

  kvStoreIt->second.ttl() = *value.ttl();
  kvStoreIt->second.ttlVersion() = *value.ttlVersion();
}

} // namespace

namespace detail {

bool
isValidTtl(int64_t val) {
  return val == Constants::kTtlInfinity || val > 0;
}

bool
isValidVersion(const int64_t myVersion, const thrift::Value& incomingVal) {
  return (incomingVal.version() > 0) && (incomingVal.version() >= myVersion);
}

MergeType
getMergeType(
    const std::string& key,
    const thrift::Value& value,
    const thrift::KeyVals& kvStore,
    std::optional<std::string> const& sender,
    thrift::KvStoreMergeResult& stats,
    const thrift::Value* curValue) {
  // Use provided current value if available, otherwise do lookup
  if (curValue == nullptr) {
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      curValue = &(kvStoreIt->second);
    }
  }

  int64_t myVersion =
      curValue ? *curValue->version() : openr::Constants::kUndefinedVersion;

  if (isTtlUpdate(value)) {
    /*
     * No value field. This is ttl refreshing coming from local store(aka,
     * self-originated key) or ttl updates coming from remote node.
     *
     * NOTE: kvStore will try to detect possible inconsistencies and force a
     * full-sync if it is adjacency peer.
     */
    if (isResyncNeeded(key, value, curValue)) {
      auto senderId = sender.value_or("");
      if (senderId == *value.originatorId()) {
        return MergeType::RESYNC_NEEDED;
      }
    } else if (curValue && *value.ttlVersion() > *curValue->ttlVersion()) {
      /*
       * Case 4: key exists, same version, same originatorId.
       */
      if (XLOG_IS_ON(DBG4)) {
        XLOG(DBG4) << fmt::format(
            "(mergeKeyValues) Update ttl key: {} with higher ttlVersion: {}, old one: {}",
            key,
            *value.ttlVersion(),
            *curValue->ttlVersion());
      }

      return MergeType::UPDATE_TTL_NEEDED;
    }
  } else {
    if (!isValidVersionAndLog(myVersion, key, value, stats)) {
      /*
       * skip if the version is invalid or old
       *
       * Attention: this condition does NOT apply to ttl. It can happen with
       * version inconsistency with ttl update.
       */
      return MergeType::NO_UPDATE_NEEDED;
    } else if (*value.version() > myVersion) {
      /*
       * [1st tie-breaker] version - prefer higher
       *
       * i) coming key version is higher
       * ii) first time seeing this key(myVersion =0)
       *
       * ATTN: for newVersion < myVersion case, util::isValidVersionAndLog()
       * has already guarded against that case. :)
       */
      XLOG(DBG4) << fmt::format(
          "(mergeKeyValues) Update key: {} with higher version: {}, old one: {}",
          key,
          *value.version(),
          myVersion);

      return MergeType::UPDATE_ALL_NEEDED;
    } else if (curValue && *value.originatorId() > *curValue->originatorId()) {
      /*
       * [2nd tie-breaker] originatorId - prefer higher
       */
      XLOG(INFO) << fmt::format(
          "(mergeKeyValues) Update key: {} with higher originatorId: {}, old one: {}",
          key,
          *value.originatorId(),
          *curValue->originatorId());

      return MergeType::UPDATE_ALL_NEEDED;
    } else if (curValue && *value.originatorId() == *curValue->originatorId()) {
      /*
       * [3rd tie-breaker] value - prefer higher
       *
       * This can occur after kvstore restarts or simply reconnects after
       * disconnection. We let one of the two values win if they differ(
       * higher in this case but can be lower as long as it's deterministic).
       * Otherwise, local store can have new value while other stores have
       * old value and they never sync.
       */

      // Note: We assume local store should always have a value. If not, it is
      // an invalid case and is ok to crash kvstore. For non-ttl update, the
      // incoming keys should always have a value associated
      auto rc = (apache::thrift::can_throw(*value.value()))
                    .compare(apache::thrift::can_throw(*curValue->value()));

      if (rc > 0) {
        // versions and orginatorIds are same but value is higher
        XLOG(DBG4) << fmt::format(
            "(mergeKeyValues) Update key: {} with higher value", key);

        return MergeType::UPDATE_ALL_NEEDED;
      } else if (rc == 0) {
        /*
         * [4th tie-breaker] ttlVersion - prefer higher
         */
        if (*value.ttlVersion() > *curValue->ttlVersion()) {
          XLOG(DBG4) << fmt::format(
              "(mergeKeyValues) Update key: {} with higher ttlVersion: {}, old one: {}",
              key,
              *value.ttlVersion(),
              *curValue->ttlVersion());

          return MergeType::UPDATE_TTL_NEEDED;
        }
      } else {
        /*
         * regarding to rc < 0 case, value in local store wins the
         * tie-breaking and no update is generated.
         */
        return MergeType::NO_UPDATE_NEEDED;
      }
    } else {
      /*
       * regarding to value.originatorId < curValue->originatorId
       * case, value in local store wins the tie-breaking and no update is
       * generated.
       */
      return MergeType::NO_UPDATE_NEEDED;
    }
  }
  return MergeType::NO_UPDATE_NEEDED;
}

void
printKeyValInArea(
    const std::string& logStr,
    const std::string& areaTag,
    const std::string& key,
    const thrift::Value& val) {
  XLOG(DBG1) << fmt::format(
      "{}{} [key: {}, v: {}, originatorId: {}, ttlVersion: {}, ttl: {}]",
      areaTag,
      logStr,
      key,
      *val.version(),
      *val.originatorId(),
      *val.ttlVersion(),
      *val.ttl());
}

} // namespace detail

thrift::KvStoreMergeResult
mergeKeyValues(
    thrift::KeyVals& kvStore,
    thrift::KeyVals const& keyVals,
    std::optional<KvStoreFilters> const& filters,
    std::optional<std::string> const& sender) {
  thrift::KvStoreMergeResult result;
  size_t nValUpdate{0};
  size_t nTtlUpdate{0};

  for (const auto& [key, value] : keyVals) {
    if (!isKeyMatchAndLog(filters, key, value, result)) {
      // skip if the filter is set and key does NOT match the filter
      continue;
    }

    if (!isValidTtlAndLog(key, value, result)) {
      // skip if the ttl is invalid
      continue;
    }

    // Do the lookup once and reuse for getMergeType and update
    auto kvStoreIt = kvStore.find(key);
    const thrift::Value* curValue =
        (kvStoreIt != kvStore.end()) ? &kvStoreIt->second : nullptr;

    const MergeType mergeType =
        detail::getMergeType(key, value, kvStore, sender, result, curValue);

    if (noNeedUpdateAndLog(mergeType, key, result)) {
      continue;
    }

    if (isInconsistentAndLog(mergeType, key, result)) {
      continue;
    }

    if (XLOG_IS_ON(DBG3)) {
      auto localVersion = curValue ? *curValue->version() : -1;
      auto localTtl = curValue ? *curValue->ttl() : -1;
      auto localTtlVersion = curValue ? *curValue->ttlVersion() : -1;
      std::string localOriginatorId =
          curValue ? *curValue->originatorId() : "null";
      XLOG(DBG3) << fmt::format(
          "Updating key: {}\n  Version: {} -> {}\n  Originator: {} -> {}\n  TtlVersion: {} -> {}\n  Ttl: {} -> {}",
          key,
          localVersion,
          *value.version(),
          localOriginatorId,
          *value.originatorId(),
          localTtlVersion,
          *value.ttlVersion(),
          localTtl,
          *value.ttl());
    }

    if (mergeType == MergeType::UPDATE_ALL_NEEDED) {
      nValUpdate++;
      updateKvStoreValue(kvStore, key, value);
    } else if (mergeType == MergeType::UPDATE_TTL_NEEDED) {
      nTtlUpdate++;
      updateKvStoreTtl(kvStore, key, value);
    }
    // announce the update
    result.keyVals()->emplace(key, value);
  }

  XLOG(DBG3) << fmt::format(
      "({}) updating {} keyvals. ValueUpdates: {}, TtlUpdates: {}",
      __FUNCTION__,
      result.keyVals()->size(),
      nValUpdate,
      nTtlUpdate);

  return result;
}

/**
 * Compare two values to find out which value is better
 */
ComparisonResult
compareValues(const thrift::Value& v1, const thrift::Value& v2) {
  // compare version
  if (*v1.version() != *v2.version()) {
    return *v1.version() > *v2.version() ? ComparisonResult::FIRST
                                         : ComparisonResult::SECOND;
  }

  // compare orginatorId
  if (*v1.originatorId() != *v2.originatorId()) {
    return *v1.originatorId() > *v2.originatorId() ? ComparisonResult::FIRST
                                                   : ComparisonResult::SECOND;
  }

  // compare value
  if (v1.hash().has_value() && v2.hash().has_value() &&
      *v1.hash() == *v2.hash()) {
    // TODO: `ttlVersion` and `ttl` value can be different on neighbor nodes.
    // The ttl-update should never be sent over the full-sync
    // hashes are same => (version, orginatorId, value are same)
    // compare ttl-version
    if (*v1.ttlVersion() != *v2.ttlVersion()) {
      return *v1.ttlVersion() > *v2.ttlVersion() ? ComparisonResult::FIRST
                                                 : ComparisonResult::SECOND;
    } else {
      return ComparisonResult::TIED;
    }
  }

  // can't use hash, either it's missing or they are different
  // compare values
  if (v1.value().has_value() && v2.value().has_value()) {
    auto compareRes = (*v1.value()).compare(*v2.value());
    if (compareRes > 0) {
      return ComparisonResult::FIRST;
    } else if (compareRes < 0) {
      return ComparisonResult::SECOND;
    } else {
      return ComparisonResult::TIED;
    }
  } else {
    // some value is missing
    return ComparisonResult::UNKNOWN;
  }
}

KvStoreFilters::KvStoreFilters(
    std::vector<std::string> const& keyPrefix,
    std::set<std::string> const& nodeIds,
    thrift::FilterOperator const& filterOperator)
    : keyPrefixList_(keyPrefix),
      originatorIds_(nodeIds),
      keyRegexSet_(RegexSet(keyPrefixList_)),
      filterOperator_(filterOperator) {}

// The function return true if there is a match on one of
// the attributes, such as key prefix or originator ids.
bool
KvStoreFilters::keyMatchAny(
    std::string const& key, thrift::Value const& value) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    // No filter and nothing to match against.
    return true;
  }
  if (!keyPrefixList_.empty() && keyRegexSet_.match(key)) {
    return true;
  }
  if (!originatorIds_.empty() && originatorIds_.count(*value.originatorId())) {
    return true;
  }
  return false;
}

// The function return true if there is a match on all the attributes
// such as key prefix and originator ids.
bool
KvStoreFilters::keyMatchAll(
    std::string const& key, thrift::Value const& value) const {
  if (keyPrefixList_.empty() && originatorIds_.empty()) {
    // No filter and nothing to match against.
    return true;
  }

  if (!keyPrefixList_.empty() && !keyRegexSet_.match(key)) {
    return false;
  }

  if (!originatorIds_.empty() && !originatorIds_.count(*value.originatorId())) {
    return false;
  }

  return true;
}

bool
KvStoreFilters::keyMatch(
    std::string const& key, thrift::Value const& value) const {
  if (filterOperator_ == thrift::FilterOperator::OR) {
    return keyMatchAny(key, value);
  }
  return keyMatchAll(key, value);
}

// The function return true if there is a key match
bool
KvStoreFilters::keyMatch(std::string const& key) const {
  if (keyPrefixList_.empty()) {
    return true;
  }
  return keyRegexSet_.match(key);
}

std::vector<std::string>
KvStoreFilters::getKeyPrefixes() const {
  return keyPrefixList_;
}

std::set<std::string>
KvStoreFilters::getOriginatorIdList() const {
  return originatorIds_;
}

std::string
KvStoreFilters::str() const {
  std::string result{};
  result += "\nPrefix filters:\n";
  for (const auto& prefixString : keyPrefixList_) {
    result += fmt::format("{}, ", prefixString);
  }
  result += "\nOriginator ID filters:\n";
  for (const auto& originatorId : originatorIds_) {
    result += fmt::format("{}, ", originatorId);
  }
  return result;
}

// dump the keys on which hashes differ from given keyVals
// thriftPub.keyVals: better keys or keys exist only in MY-KEY-VAL
// thriftPub.tobeUpdatedKeys: better keys or keys exist only in REQ-KEY-VAL
// this way, full-sync initiator knows what keys need to send back to finish
// 3-way full-sync
thrift::Publication
dumpDifference(
    const std::string& area,
    const thrift::KeyVals& myKeyVal,
    const thrift::KeyVals& reqKeyVal) {
  thrift::Publication thriftPub;
  thriftPub.area() = area;

  thriftPub.tobeUpdatedKeys() = std::vector<std::string>{};

  for (const auto& [myKey, myVal] : myKeyVal) {
    const auto& reqKv = reqKeyVal.find(myKey);

    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals()->emplace(myKey, myVal);
      continue;
    }

    const auto& reqVal = reqKv->second;
    ComparisonResult rc = compareValues(myVal, reqVal);

    if (rc == ComparisonResult::FIRST || rc == ComparisonResult::UNKNOWN) {
      thriftPub.keyVals()->emplace(myKey, myVal);
    }
    if (rc == ComparisonResult::SECOND || rc == ComparisonResult::UNKNOWN) {
      thriftPub.tobeUpdatedKeys()->emplace_back(myKey);
    }
  }

  for (const auto& [reqKey, reqVal] : reqKeyVal) {
    const auto& myKv = myKeyVal.find(reqKey);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys()->emplace_back(reqKey);
    }
  }

  return thriftPub;
}

// dump the entries of my KV store whose keys match filter
// KvStoreFilters contains `thrift::FilterOperator`
// Default to thrift::FilterOperator::OR
thrift::Publication
dumpAllWithFilters(
    const std::string& area,
    const thrift::KeyVals& kvStore,
    const KvStoreFilters& kvFilters,
    bool doNotPublishValue) {
  thrift::Publication thriftPub;
  thriftPub.area() = area;

  for (auto const& [key, val] : kvStore) {
    if (!kvFilters.keyMatch(key, val)) {
      continue;
    }
    if (!doNotPublishValue) {
      thriftPub.keyVals()[key] = val;
    } else {
      thriftPub.keyVals()[key] = createThriftValueWithoutBinaryValue(val);
    }
  }

  return thriftPub;
}

// dump the hashes of my KV store whose keys match the given prefix
// if prefix is the empty string, the full hash store is dumped
thrift::Publication
dumpHashWithFilters(
    const std::string& area,
    const thrift::KeyVals& kvStore,
    const KvStoreFilters& kvFilters) {
  thrift::Publication thriftPub;
  thriftPub.area() = area;
  for (auto const& [key, val] : kvStore) {
    if (!kvFilters.keyMatch(key, val)) {
      continue;
    }
    DCHECK(val.hash().has_value());
    auto& value = thriftPub.keyVals()[key];
    value.version() = *val.version();
    value.originatorId() = *val.originatorId();
    value.hash().copy_from(val.hash());
    value.ttl() = *val.ttl();
    value.ttlVersion() = *val.ttlVersion();
  }
  return thriftPub;
}
// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
updatePublicationTtl(
    const TtlCountdownQueue& ttlCountdownQueue,
    const std::chrono::milliseconds ttlDecr,
    thrift::Publication& thriftPub,
    const bool removeAboutToExpire) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals()->find(qE.key);
    if (kv == thriftPub.keyVals()->end() ||
        *kv->second.version() != qE.version ||
        *kv->second.originatorId() != qE.originatorId ||
        *kv->second.ttlVersion() != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(
        qE.expiryTime - timeNow);
    if (timeLeft <= ttlDecr) {
      thriftPub.keyVals()->erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (removeAboutToExpire && (timeLeft < Constants::kTtlThreshold)) {
      thriftPub.keyVals()->erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This
    // will avoid looping of updates between stores.
    kv->second.ttl() = timeLeft.count() - ttlDecr.count();
  }
}

std::string
getAreaTypeByAreaName(const std::string& area) {
  if (re2::RE2::FullMatch(area, re2::RE2(Constants::podEndingPattern))) {
    return "POD";
  } else if (re2::RE2::FullMatch(
                 area, re2::RE2(Constants::spineEndingPattern))) {
    return "SPINE";
  } else if (re2::RE2::FullMatch(
                 area, re2::RE2(Constants::sliceEndingPattern))) {
    return "SLICE";
  } else if (re2::RE2::FullMatch(
                 area, re2::RE2(Constants::hgridEndingPattern))) {
    return "HGRID";
  }
  // default
  return "UNKNOWN";
}
} // namespace openr
