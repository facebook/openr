/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/kvstore/KvStoreUtil.h>

namespace openr {

std::optional<openr::KvStoreFilters>
getKvStoreFilters(const thrift::KvStoreConfig& kvStoreConfig) {
  std::optional<openr::KvStoreFilters> kvFilters{std::nullopt};
  // Add key prefixes to allow if set as leaf node
  if (kvStoreConfig.set_leaf_node().value_or(false)) {
    std::vector<std::string> keyPrefixFilters;
    if (auto v = kvStoreConfig.key_prefix_filters()) {
      keyPrefixFilters = *v;
    }
    keyPrefixFilters.push_back(openr::Constants::kPrefixAllocMarker.toString());
    keyPrefixFilters.push_back(
        openr::Constants::kNodeLabelRangePrefix.toString());

    // save nodeIds in the set
    std::set<std::string> originatorIdFilters{};
    for (const auto& id :
         kvStoreConfig.key_originator_id_filters().value_or({})) {
      originatorIdFilters.insert(id);
    }
    originatorIdFilters.insert(*kvStoreConfig.node_name());
    kvFilters = openr::KvStoreFilters(keyPrefixFilters, originatorIdFilters);
  }
  return kvFilters;
}

std::pair<
    std::unordered_map<std::string, thrift::Value>,
    KvStoreNoMergeReasonStats>
mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    std::optional<KvStoreFilters> const& filters,
    std::optional<std::string> const& sender) {
  // the publication to build if we update our KV store
  std::unordered_map<std::string, thrift::Value> kvUpdates;
  KvStoreNoMergeReasonStats stats;

  // Counters for logging
  uint32_t ttlUpdateCnt{0}, valUpdateCnt{0};

  for (const auto& [key, value] : keyVals) {
    if (filters.has_value() && not filters->keyMatch(key, value)) {
      XLOG(DBG4) << "key: " << key << " not adding from "
                 << *value.originatorId();
      stats.noMergeReasons.emplace(key, KvStoreNoMergeReason::NO_MATCHED_KEY);
      ++stats.numberOfNoMatchedKeys;
      continue;
    }

    // versions must start at 1; setting this to zero here means
    // we would be beaten by any version supplied by the setter
    int64_t myVersion{0};
    int64_t newVersion = *value.version();

    // Check if TTL is valid. It must be infinite or positive number
    // Skip if invalid!
    if (*value.ttl() != Constants::kTtlInfinity && *value.ttl() <= 0) {
      stats.noMergeReasons.emplace(key, KvStoreNoMergeReason::INVALID_TTL);
      stats.listInvalidTtls.push_back(*value.ttl());
      continue;
    }

    // if key exist, compare values first
    // if they are the same, no need to propagate changes
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      myVersion = *kvStoreIt->second.version();
    } else {
      XLOG(DBG4) << "(mergeKeyValues) key: '" << key << "' not found, adding";
    }

    // TTL update but version is not the same
    // One version update is missed and there is inconsistency. Resync
    if (newVersion != myVersion and not value.value().has_value()) {
      auto originator = *value.originatorId();
      auto senderName = sender.value_or("");
      XLOG(ERR) << fmt::format(
          "(mergeKeyValues) Received ttl update from {}. key: {}, received version: {}, Local version: {}, originator: {}",
          senderName,
          key,
          myVersion,
          newVersion,
          originator);
      // if sender is the key originator, then stop and report (and trigger
      // resync) Triggering resync with originator will help originator to bump
      // its key version if needed. However, if the sender is not the originator
      // of the inconsistent key, resync will not help. E.g A-B-C. if A is the
      // originator of the key, and C receives an inconsistent update from B C
      // will not ask to resync with B.
      if (senderName == originator) {
        stats.inconsistencyDetetectedWithOriginator = true;
        return std::make_pair(std::move(kvUpdates), std::move(stats));
      }
    }

    // If we get an old value just skip it
    if (newVersion < myVersion) {
      stats.noMergeReasons.emplace(key, KvStoreNoMergeReason::OLD_VERSION);
      stats.listOldVersions.push_back(newVersion);
      continue;
    }

    bool updateAllNeeded{false};
    bool updateTtlNeeded{false};

    //
    // Check updateAll and updateTtl
    //
    if (value.value().has_value()) {
      if (newVersion > myVersion) {
        // Version is newer or
        // kvStoreIt is NULL(myVersion is set to 0)
        updateAllNeeded = true;
      } else if (*value.originatorId() > *kvStoreIt->second.originatorId()) {
        // versions are the same but originatorId is higher
        updateAllNeeded = true;
      } else if (*value.originatorId() == *kvStoreIt->second.originatorId()) {
        // This can occur after kvstore restarts or simply reconnects after
        // disconnection. We let one of the two values win if they
        // differ(higher in this case but can be lower as long as it's
        // deterministic). Otherwise, local store can have new value while
        // other stores have old value and they never sync.
        int rc = (*value.value()).compare(*kvStoreIt->second.value());
        if (rc > 0) {
          // versions and orginatorIds are same but value is higher
          XLOG(DBG3) << "Previous incarnation reflected back for key " << key;
          updateAllNeeded = true;
        } else if (rc == 0) {
          // versions, orginatorIds, value are all same
          // retain higher ttlVersion
          if (*value.ttlVersion() > *kvStoreIt->second.ttlVersion()) {
            updateTtlNeeded = true;
          }
        }
      }
    }

    //
    // Check updateTtl
    //
    if (not value.value().has_value() and kvStoreIt != kvStore.end() and
        *value.version() == *kvStoreIt->second.version() and
        *value.originatorId() == *kvStoreIt->second.originatorId() and
        *value.ttlVersion() > *kvStoreIt->second.ttlVersion()) {
      updateTtlNeeded = true;
    }

    if (!updateAllNeeded and !updateTtlNeeded) {
      XLOG(DBG3) << "(mergeKeyValues) no need to update anything for key: '"
                 << key << "'";
      stats.noMergeReasons.emplace(
          key, KvStoreNoMergeReason::NO_NEED_TO_UPDATE);
      ++stats.numberOfNoNeedToUpdates;
      continue;
    }

    XLOG(DBG3)
        << "Updating key: " << key << "\n  Version: " << myVersion << " -> "
        << newVersion << "\n  Originator: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.originatorId()
                                       : "null")
        << " -> " << *value.originatorId() << "\n  TtlVersion: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.ttlVersion() : 0)
        << " -> " << *value.ttlVersion() << "\n  Ttl: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.ttl() : 0) << " -> "
        << *value.ttl();

    // grab the new value (this will copy, intended)
    thrift::Value newValue = value;

    if (updateAllNeeded) {
      ++valUpdateCnt;
      FB_LOG_EVERY_MS(INFO, 500) << "Updating key: " << key
                                 << ", Originator: " << *value.originatorId()
                                 << ", Version: " << newVersion
                                 << ", TtlVersion: " << *value.ttlVersion()
                                 << ", Ttl: " << *value.ttl();
      //
      // update everything for such key
      //
      CHECK(value.value().has_value());
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
      if (not kvStoreIt->second.hash().has_value()) {
        kvStoreIt->second.hash() = generateHash(
            *value.version(), *value.originatorId(), value.value());
      }
    } else if (updateTtlNeeded) {
      ++ttlUpdateCnt;
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl() = *value.ttl();
      kvStoreIt->second.ttlVersion() = *value.ttlVersion();
    }

    // announce the update
    kvUpdates.emplace(key, value);
  }

  XLOG(DBG4) << "(mergeKeyValues) updating " << kvUpdates.size()
             << " keyvals. ValueUpdates: " << valUpdateCnt
             << ", TtlUpdates: " << ttlUpdateCnt;
  return std::make_pair(std::move(kvUpdates), std::move(stats));
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
  if (v1.hash().has_value() and v2.hash().has_value() and
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
  if (v1.value().has_value() and v2.value().has_value()) {
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

  if (!keyPrefixList_.empty() && not keyRegexSet_.match(key)) {
    return false;
  }

  if (!originatorIds_.empty() &&
      not originatorIds_.count(*value.originatorId())) {
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
    std::unordered_map<std::string, thrift::Value> const& myKeyVal,
    std::unordered_map<std::string, thrift::Value> const& reqKeyVal) {
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

    if (rc == ComparisonResult::FIRST or rc == ComparisonResult::UNKNOWN) {
      thriftPub.keyVals()->emplace(myKey, myVal);
    }
    if (rc == ComparisonResult::SECOND or rc == ComparisonResult::UNKNOWN) {
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
    const std::unordered_map<std::string, thrift::Value>& kvStore,
    const KvStoreFilters& kvFilters,
    bool doNotPublishValue) {
  thrift::Publication thriftPub;
  thriftPub.area() = area;

  for (auto const& [key, val] : kvStore) {
    if (not kvFilters.keyMatch(key, val)) {
      continue;
    }
    if (not doNotPublishValue) {
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
    const std::unordered_map<std::string, thrift::Value>& kvStore,
    const KvStoreFilters& kvFilters) {
  thrift::Publication thriftPub;
  thriftPub.area() = area;
  for (auto const& [key, val] : kvStore) {
    if (not kvFilters.keyMatch(key, val)) {
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
    if (kv == thriftPub.keyVals()->end() or
        *kv->second.version() != qE.version or
        *kv->second.originatorId() != qE.originatorId or
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
    if (removeAboutToExpire and (timeLeft < Constants::kTtlThreshold)) {
      thriftPub.keyVals()->erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This
    // will avoid looping of updates between stores.
    kv->second.ttl() = timeLeft.count() - ttlDecr.count();
  }
}

}; // namespace openr
