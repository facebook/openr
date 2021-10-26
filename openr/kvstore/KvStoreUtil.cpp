/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/logging/xlog.h>

#include <openr/common/Constants.h>
#include <openr/kvstore/KvStoreUtil.h>

namespace openr {

std::optional<openr::KvStoreFilters>
getKvStoreFilters(std::shared_ptr<const openr::Config> config) {
  std::optional<openr::KvStoreFilters> kvFilters{std::nullopt};
  // Add key prefixes to allow if set as leaf node
  if (config->getKvStoreConfig().set_leaf_node_ref().value_or(false)) {
    std::vector<std::string> keyPrefixFilters;
    if (auto v = config->getKvStoreConfig().key_prefix_filters_ref()) {
      keyPrefixFilters = *v;
    }
    keyPrefixFilters.push_back(openr::Constants::kPrefixAllocMarker.toString());
    keyPrefixFilters.push_back(
        openr::Constants::kNodeLabelRangePrefix.toString());

    // save nodeIds in the set
    std::set<std::string> originatorIdFilters{};
    for (const auto& id :
         config->getKvStoreConfig().key_originator_id_filters_ref().value_or(
             {})) {
      originatorIdFilters.insert(id);
    }
    originatorIdFilters.insert(config->getNodeName());
    kvFilters = openr::KvStoreFilters(keyPrefixFilters, originatorIdFilters);
  }
  return kvFilters;
}

std::unordered_map<std::string, thrift::Value>
mergeKeyValues(
    std::unordered_map<std::string, thrift::Value>& kvStore,
    std::unordered_map<std::string, thrift::Value> const& keyVals,
    std::optional<KvStoreFilters> const& filters) {
  // the publication to build if we update our KV store
  std::unordered_map<std::string, thrift::Value> kvUpdates;

  // Counters for logging
  uint32_t ttlUpdateCnt{0}, valUpdateCnt{0};

  for (const auto& [key, value] : keyVals) {
    if (filters.has_value() && not filters->keyMatch(key, value)) {
      XLOG(DBG4) << "key: " << key << " not adding from "
                 << *value.originatorId_ref();
      continue;
    }

    // versions must start at 1; setting this to zero here means
    // we would be beaten by any version supplied by the setter
    int64_t myVersion{0};
    int64_t newVersion = *value.version_ref();

    // Check if TTL is valid. It must be infinite or positive number
    // Skip if invalid!
    if (*value.ttl_ref() != Constants::kTtlInfinity && *value.ttl_ref() <= 0) {
      continue;
    }

    // if key exist, compare values first
    // if they are the same, no need to propagate changes
    auto kvStoreIt = kvStore.find(key);
    if (kvStoreIt != kvStore.end()) {
      myVersion = *kvStoreIt->second.version_ref();
    } else {
      XLOG(DBG4) << "(mergeKeyValues) key: '" << key << "' not found, adding";
    }

    // If we get an old value just skip it
    if (newVersion < myVersion) {
      continue;
    }

    bool updateAllNeeded{false};
    bool updateTtlNeeded{false};

    //
    // Check updateAll and updateTtl
    //
    if (value.value_ref().has_value()) {
      if (newVersion > myVersion) {
        // Version is newer or
        // kvStoreIt is NULL(myVersion is set to 0)
        updateAllNeeded = true;
      } else if (
          *value.originatorId_ref() > *kvStoreIt->second.originatorId_ref()) {
        // versions are the same but originatorId is higher
        updateAllNeeded = true;
      } else if (
          *value.originatorId_ref() == *kvStoreIt->second.originatorId_ref()) {
        // This can occur after kvstore restarts or simply reconnects after
        // disconnection. We let one of the two values win if they
        // differ(higher in this case but can be lower as long as it's
        // deterministic). Otherwise, local store can have new value while
        // other stores have old value and they never sync.
        int rc = (*value.value_ref()).compare(*kvStoreIt->second.value_ref());
        if (rc > 0) {
          // versions and orginatorIds are same but value is higher
          XLOG(DBG3) << "Previous incarnation reflected back for key " << key;
          updateAllNeeded = true;
        } else if (rc == 0) {
          // versions, orginatorIds, value are all same
          // retain higher ttlVersion
          if (*value.ttlVersion_ref() > *kvStoreIt->second.ttlVersion_ref()) {
            updateTtlNeeded = true;
          }
        }
      }
    }

    //
    // Check updateTtl
    //
    if (not value.value_ref().has_value() and kvStoreIt != kvStore.end() and
        *value.version_ref() == *kvStoreIt->second.version_ref() and
        *value.originatorId_ref() == *kvStoreIt->second.originatorId_ref() and
        *value.ttlVersion_ref() > *kvStoreIt->second.ttlVersion_ref()) {
      updateTtlNeeded = true;
    }

    if (!updateAllNeeded and !updateTtlNeeded) {
      XLOG(DBG3) << "(mergeKeyValues) no need to update anything for key: '"
                 << key << "'";
      continue;
    }

    XLOG(DBG3)
        << "Updating key: " << key << "\n  Version: " << myVersion << " -> "
        << newVersion << "\n  Originator: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.originatorId_ref()
                                       : "null")
        << " -> " << *value.originatorId_ref() << "\n  TtlVersion: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.ttlVersion_ref()
                                       : 0)
        << " -> " << *value.ttlVersion_ref() << "\n  Ttl: "
        << (kvStoreIt != kvStore.end() ? *kvStoreIt->second.ttl_ref() : 0)
        << " -> " << *value.ttl_ref();

    // grab the new value (this will copy, intended)
    thrift::Value newValue = value;

    if (updateAllNeeded) {
      ++valUpdateCnt;
      FB_LOG_EVERY_MS(INFO, 500)
          << "Updating key: " << key
          << ", Originator: " << *value.originatorId_ref()
          << ", Version: " << newVersion
          << ", TtlVersion: " << *value.ttlVersion_ref()
          << ", Ttl: " << *value.ttl_ref();
      //
      // update everything for such key
      //
      CHECK(value.value_ref().has_value());
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
      if (not kvStoreIt->second.hash_ref().has_value()) {
        kvStoreIt->second.hash_ref() = generateHash(
            *value.version_ref(), *value.originatorId_ref(), value.value_ref());
      }
    } else if (updateTtlNeeded) {
      ++ttlUpdateCnt;
      //
      // update ttl,ttlVersion only
      //
      CHECK(kvStoreIt != kvStore.end());

      // update TTL only, nothing else
      kvStoreIt->second.ttl_ref() = *value.ttl_ref();
      kvStoreIt->second.ttlVersion_ref() = *value.ttlVersion_ref();
    }

    // announce the update
    kvUpdates.emplace(key, value);
  }

  XLOG(DBG4) << "(mergeKeyValues) updating " << kvUpdates.size()
             << " keyvals. ValueUpdates: " << valUpdateCnt
             << ", TtlUpdates: " << ttlUpdateCnt;
  return kvUpdates;
}

/**
 * Compare two values to find out which value is better
 */
int
compareValues(const thrift::Value& v1, const thrift::Value& v2) {
  // compare version
  if (*v1.version_ref() != *v2.version_ref()) {
    return *v1.version_ref() > *v2.version_ref() ? 1 : -1;
  }

  // compare orginatorId
  if (*v1.originatorId_ref() != *v2.originatorId_ref()) {
    return *v1.originatorId_ref() > *v2.originatorId_ref() ? 1 : -1;
  }

  // compare value
  if (v1.hash_ref().has_value() and v2.hash_ref().has_value() and
      *v1.hash_ref() == *v2.hash_ref()) {
    // TODO: `ttlVersion` and `ttl` value can be different on neighbor nodes.
    // The ttl-update should never be sent over the full-sync
    // hashes are same => (version, orginatorId, value are same)
    // compare ttl-version
    if (*v1.ttlVersion_ref() != *v2.ttlVersion_ref()) {
      return *v1.ttlVersion_ref() > *v2.ttlVersion_ref() ? 1 : -1;
    } else {
      return 0;
    }
  }

  // can't use hash, either it's missing or they are different
  // compare values
  if (v1.value_ref().has_value() and v2.value_ref().has_value()) {
    return (*v1.value_ref()).compare(*v2.value_ref());
  } else {
    // some value is missing
    return -2; // unknown
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
  if (!originatorIds_.empty() &&
      originatorIds_.count(*value.originatorId_ref())) {
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
      not originatorIds_.count(*value.originatorId_ref())) {
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
  thriftPub.area_ref() = area;

  thriftPub.tobeUpdatedKeys_ref() = std::vector<std::string>{};

  for (const auto& [myKey, myVal] : myKeyVal) {
    const auto& reqKv = reqKeyVal.find(myKey);

    if (reqKv == reqKeyVal.end()) {
      // not exist in reqKeyVal
      thriftPub.keyVals_ref()->emplace(myKey, myVal);
      continue;
    }

    const auto& reqVal = reqKv->second;
    int rc = compareValues(myVal, reqVal);

    if (rc == 1 or rc == -2) {
      // myVal is better or unknown
      thriftPub.keyVals_ref()->emplace(myKey, myVal);
    }
    if (rc == -1 or rc == -2) {
      // reqVal is better or unknown
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(myKey);
    }
  }

  for (const auto& [reqKey, reqVal] : reqKeyVal) {
    const auto& myKv = myKeyVal.find(reqKey);
    if (myKv == myKeyVal.end()) {
      // not exist in myKeyVal
      thriftPub.tobeUpdatedKeys_ref()->emplace_back(reqKey);
    }
  }

  return thriftPub;
}

// dump the entries of my KV store whose keys match filter
thrift::Publication
dumpAllWithFilters(
    const std::string& area,
    const std::unordered_map<std::string, thrift::Value>& kvStore,
    const KvStoreFilters& kvFilters,
    thrift::FilterOperator oper,
    bool doNotPublishValue) {
  thrift::Publication thriftPub;
  thriftPub.area_ref() = area;

  // TODO: simplify the operations to OR case only in accordance with practice
  switch (oper) {
  case thrift::FilterOperator::AND:
    for (auto const& [key, val] : kvStore) {
      if (not kvFilters.keyMatchAll(key, val)) {
        continue;
      }
      if (not doNotPublishValue) {
        thriftPub.keyVals_ref()[key] = val;
      } else {
        thriftPub.keyVals_ref()[key] = createThriftValueWithoutBinaryValue(val);
      }
    }
    break;
  default:
    for (auto const& [key, val] : kvStore) {
      if (not kvFilters.keyMatch(key, val)) {
        continue;
      }
      if (not doNotPublishValue) {
        thriftPub.keyVals_ref()[key] = val;
      } else {
        thriftPub.keyVals_ref()[key] = createThriftValueWithoutBinaryValue(val);
      }
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
  thriftPub.area_ref() = area;
  for (auto const& [key, val] : kvStore) {
    if (not kvFilters.keyMatch(key, val)) {
      continue;
    }
    DCHECK(val.hash_ref().has_value());
    auto& value = thriftPub.keyVals_ref()[key];
    value.version_ref() = *val.version_ref();
    value.originatorId_ref() = *val.originatorId_ref();
    value.hash_ref().copy_from(val.hash_ref());
    value.ttl_ref() = *val.ttl_ref();
    value.ttlVersion_ref() = *val.ttlVersion_ref();
  }
  return thriftPub;
}
// update TTL with remainng time to expire, TTL version remains
// same so existing keys will not be updated with this TTL
void
updatePublicationTtl(
    const TtlCountdownQueue& ttlCountdownQueue,
    const std::chrono::milliseconds ttlDecr,
    thrift::Publication& thriftPub) {
  auto timeNow = std::chrono::steady_clock::now();
  for (const auto& qE : ttlCountdownQueue) {
    // Find key and ensure we are taking time from right entry from queue
    auto kv = thriftPub.keyVals_ref()->find(qE.key);
    if (kv == thriftPub.keyVals_ref()->end() or
        *kv->second.version_ref() != qE.version or
        *kv->second.originatorId_ref() != qE.originatorId or
        *kv->second.ttlVersion_ref() != qE.ttlVersion) {
      continue;
    }

    // Compute timeLeft and do sanity check on it
    auto timeLeft = std::chrono::duration_cast<std::chrono::milliseconds>(
        qE.expiryTime - timeNow);
    if (timeLeft <= ttlDecr) {
      thriftPub.keyVals_ref()->erase(kv);
      continue;
    }

    // filter key from publication if time left is below ttl threshold
    if (timeLeft < Constants::kTtlThreshold) {
      thriftPub.keyVals_ref()->erase(kv);
      continue;
    }

    // Set the time-left and decrement it by one so that ttl decrement
    // deterministically whenever it is exchanged between KvStores. This
    // will avoid looping of updates between stores.
    kv->second.ttl_ref() = timeLeft.count() - ttlDecr.count();
  }
}

}; // namespace openr
