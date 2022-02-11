/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>
#include <openr/common/Util.h>
#include <openr/kvstore/KvStoreClientInternal.h>

namespace fb303 = facebook::fb303;

namespace openr {

KvStoreClientInternal::KvStoreClientInternal(
    OpenrEventBase* eventBase, std::string const& nodeId, KvStore* kvStore)
    : nodeId_(nodeId), eventBase_(eventBase), kvStore_(kvStore) {
  // sanity check
  CHECK_NE(eventBase_, static_cast<void*>(nullptr));
  CHECK(not nodeId.empty());
  CHECK(kvStore_);

  // Fiber to process thrift::Publication from KvStore
  taskFuture_ = eventBase_->addFiberTaskFuture(
      [q = std::move(kvStore_->getKvStoreUpdatesReader()),
       this]() mutable noexcept {
        XLOG(INFO) << "Starting KvStore updates processing fiber";
        while (true) {
          auto maybePub = q.get(); // perform read
          if (maybePub.hasError()) {
            XLOG(INFO) << "Terminating KvStore updates processing fiber";
            break;
          }

          folly::variant_match(
              std::move(maybePub).value(),
              [this](thrift::Publication&& pub) {
                processPublication(std::move(pub));
              },
              [](thrift::InitializationEvent&&) {
                // Do not interested in initialization event
              });
        }
      });

  // create throttled fashion of ttl update
  advertiseTtlUpdatesThrottled_ = std::make_unique<AsyncThrottle>(
      eventBase_->getEvb(),
      Constants::kKvStoreSyncThrottleTimeout,
      [this]() noexcept { advertiseTtlUpdates(); });

  // initialize timers
  ttlTimer_ = folly::AsyncTimeout::make(
      *eventBase_->getEvb(), [this]() noexcept { advertiseTtlUpdates(); });
}

KvStoreClientInternal::~KvStoreClientInternal() {
  // - If EventBase is stopped or it is within the evb thread, run immediately;
  // - Otherwise, will wait the EventBase to run;
  eventBase_->getEvb()->runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    ttlTimer_.reset();
    advertiseTtlUpdatesThrottled_.reset();
  });

  // Stop kvstore internal if not stopped yet
  stop();
}

void
KvStoreClientInternal::stop() {
  // wait for fiber to be closed before destroy KvStoreClientInternal
  taskFuture_.cancel();
  taskFuture_.wait();
}

std::optional<folly::Unit>
KvStoreClientInternal::setKey(
    AreaId const& area,
    std::string const& key,
    std::string const& value,
    uint32_t version /* = 0 */,
    std::chrono::milliseconds ttl /* = Constants::kTtlInfInterval */) {
  // Create 'thrift::Value' object which will be sent to KvStore
  thrift::Value thriftValue = createThriftValue(
      version, nodeId_, value, ttl.count(), 0 /* ttl version */, 0 /* hash */);
  CHECK(thriftValue.value_ref());

  // Use one version number higher than currently in KvStore if not specified
  if (not version) {
    auto maybeValue = getKey(area, key);
    if (maybeValue.has_value()) {
      thriftValue.version_ref() = *maybeValue->version_ref() + 1;
    } else {
      thriftValue.version_ref() = 1;
    }
  }
  return setKey(area, key, thriftValue);
}

std::optional<folly::Unit>
KvStoreClientInternal::setKey(
    AreaId const& area,
    std::string const& key,
    thrift::Value const& thriftValue) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(thriftValue.value_ref());

  XLOG(DBG3) << "KvStoreClientInternal: setKey called for key " << key;

  std::unordered_map<std::string, thrift::Value> keyVals;
  keyVals.emplace(key, thriftValue);

  const auto ret = setKeysHelper(area, std::move(keyVals));

  scheduleTtlUpdates(
      area,
      key,
      *thriftValue.version_ref(),
      *thriftValue.ttlVersion_ref(),
      *thriftValue.ttl_ref(),
      false /* advertiseImmediately */);

  return ret;
}

void
KvStoreClientInternal::scheduleTtlUpdates(
    AreaId const& area,
    std::string const& key,
    uint32_t version,
    uint32_t ttlVersion,
    int64_t ttl,
    bool advertiseImmediately) {
  // infinite TTL does not need update
  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  if (ttl == Constants::kTtlInfinity) {
    // in case ttl is finite before
    keyTtlBackoffs.erase(key);
    return;
  }

  // do not send value to reduce update overhead
  thrift::Value ttlThriftValue = createThriftValue(
      version,
      nodeId_,
      std::nullopt, /* value */
      ttl, /* ttl */
      ttlVersion /* ttl version */);
  CHECK(not ttlThriftValue.value_ref().has_value());

  // renew before Ttl expires about every ttl/3, i.e., try twice
  // use ExponentialBackoff to track remaining time
  keyTtlBackoffs[key] = std::make_pair(
      ttlThriftValue,
      ExponentialBackoff<std::chrono::milliseconds>(
          std::chrono::milliseconds(ttl / 4),
          std::chrono::milliseconds(ttl / 4 + 1)));

  // Delay first ttl advertisement by (ttl / 4). We have just advertised key or
  // update and would like to avoid sending unncessary immediate ttl update
  if (not advertiseImmediately) {
    keyTtlBackoffs.at(key).second.reportError();
  }

  // ATTN: always use throttled fashion for ttl update
  advertiseTtlUpdatesThrottled_->operator()();
}

void
KvStoreClientInternal::unsetKey(AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  XLOG(DBG3) << "KvStoreClientInternal: unsetKey called for key " << key
             << " area " << area.t;

  keyTtlBackoffs_[area].erase(key);
}

std::optional<thrift::Value>
KvStoreClientInternal::getKey(AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  XLOG(DBG3) << "KvStoreClientInternal: getKey called for key " << key
             << ", area " << area.t;

  thrift::Publication pub;
  try {
    thrift::KeyGetParams params;
    params.keys_ref()->emplace_back(key);
    pub = *(kvStore_->semifuture_getKvStoreKeyVals(area, params).get());
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to get keyvals from kvstore. Exception: " << ex.what();
    return std::nullopt;
  }
  XLOG(DBG3) << "Received " << pub.keyVals_ref()->size() << " key-vals.";

  auto it = pub.keyVals_ref()->find(key);
  if (it == pub.keyVals_ref()->end()) {
    XLOG(DBG2) << "Key: " << key << " NOT found in kvstore. Area: " << area.t;
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::unordered_map<std::string, thrift::Value>>
KvStoreClientInternal::dumpAllWithPrefix(
    AreaId const& area, const std::string& prefix /* = "" */) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  thrift::Publication pub;
  try {
    thrift::KeyDumpParams params;
    *params.prefix_ref() = prefix;
    if (not prefix.empty()) {
      params.keys_ref() = {prefix};
    }
    pub = *kvStore_->semifuture_dumpKvStoreKeys(std::move(params), {area})
               .get()
               ->begin();
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to add peers to kvstore. Exception: " << ex.what();
    return std::nullopt;
  }
  return *pub.keyVals_ref();
}

std::optional<thrift::Value>
KvStoreClientInternal::subscribeKey(
    AreaId const& area,
    std::string const& key,
    KeyCallback callback,
    bool fetchKeyValue) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());
  CHECK(bool(callback)) << "Callback function for " << key << " is empty";

  XLOG(DBG3) << "KvStoreClientInternal: subscribeKey called for key " << key;
  keyCallbacks_[area][key] = std::move(callback);

  return fetchKeyValue ? getKey(area, key) : std::nullopt;
}

void
KvStoreClientInternal::subscribeKeyFilter(
    KvStoreFilters kvFilters, KeyCallback callback) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  keyPrefixFilter_ = std::move(kvFilters);
  keyPrefixFilterCallback_ = std::move(callback);
  return;
}

void
KvStoreClientInternal::unsubscribeKeyFilter() {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  keyPrefixFilterCallback_ = nullptr;
  keyPrefixFilter_ = KvStoreFilters({}, {});
  return;
}

void
KvStoreClientInternal::unsubscribeKey(
    AreaId const& area, std::string const& key) {
  CHECK(eventBase_->getEvb()->isInEventBaseThread());

  XLOG(DBG3) << "KvStoreClientInternal: unsubscribeKey called for key " << key;
  // Store callback into KeyCallback map
  if (keyCallbacks_[area].erase(key) == 0) {
    XLOG(WARNING) << "UnsubscribeKey called for non-existing key" << key;
  }
}

void
KvStoreClientInternal::processExpiredKeys(
    thrift::Publication const& publication) {
  auto const& expiredKeys = *publication.expiredKeys_ref();

  // NOTE: default construct empty map if it didn't exist
  auto& callbacks = keyCallbacks_[AreaId{*publication.area_ref()}];
  for (auto const& key : expiredKeys) {
    /* key specific registered callback */
    auto cb = callbacks.find(key);
    if (cb != callbacks.end()) {
      (cb->second)(key, std::nullopt);
    }
  }
}

void
KvStoreClientInternal::processPublication(
    thrift::Publication const& publication) {
  // Go through received key-values and find out the ones which need update
  CHECK(not publication.area_ref()->empty());
  AreaId area{*publication.area_ref()};

  // NOTE: default construct empty containers if they didn't exist
  auto& keyTtlBackoffs = keyTtlBackoffs_[area];
  auto& callbacks = keyCallbacks_[area];

  for (auto const& [key, rcvdValue] : *publication.keyVals_ref()) {
    if (not rcvdValue.value_ref().has_value()) {
      // ignore TTL update
      continue;
    }

    // Update local keyVals as per need
    auto cb = callbacks.find(key);
    // set key w/ finite TTL
    auto sk = keyTtlBackoffs.find(key);

    // key set but not persisted
    if (sk != keyTtlBackoffs.end()) {
      auto& setValue = sk->second.first;
      if (*rcvdValue.version_ref() > *setValue.version_ref() or
          (*rcvdValue.version_ref() == *setValue.version_ref() and
           *rcvdValue.originatorId_ref() > *setValue.originatorId_ref())) {
        // key lost, cancel TTL update
        keyTtlBackoffs.erase(sk);
      } else if (
          *rcvdValue.version_ref() == *setValue.version_ref() and
          *rcvdValue.originatorId_ref() == *setValue.originatorId_ref() and
          *rcvdValue.ttlVersion_ref() > *setValue.ttlVersion_ref()) {
        // If version, value and originatorId is same then we should look up
        // ttlVersion and update local value if rcvd ttlVersion is higher
        // NOTE: We don't need to advertise the value back
        if (sk != keyTtlBackoffs.end() and
            *sk->second.first.ttlVersion_ref() < *rcvdValue.ttlVersion_ref()) {
          XLOG(DBG2) << fmt::format(
                            "Bumping TTL version for [key: {}, v: {}, "
                            "originatorId: {}]",
                            key,
                            *rcvdValue.version_ref(),
                            *rcvdValue.originatorId_ref())
                     << " to " << (*rcvdValue.ttlVersion_ref() + 1) << " from "
                     << *setValue.ttlVersion_ref();
          setValue.ttlVersion_ref() = *rcvdValue.ttlVersion_ref() + 1;
        }
      }
    }

    // We need to alert callback if a key is not persisted and we
    // received a change notification for it.
    if (cb != callbacks.end()) {
      (cb->second)(key, rcvdValue);
    }
    // callback for a given key filter
    if (keyPrefixFilterCallback_ && keyPrefixFilter_.keyMatch(key, rcvdValue)) {
      keyPrefixFilterCallback_(key, rcvdValue);
    }
  } // for

  if (publication.expiredKeys_ref()->size()) {
    processExpiredKeys(publication);
  }
}

void
KvStoreClientInternal::advertiseTtlUpdates() {
  // Build set of keys to advertise ttl updates
  auto timeout = Constants::kMaxTtlUpdateInterval;

  // advertise TTL updates for each area
  for (auto& [area, keyTtlBackoffs] : keyTtlBackoffs_) {
    std::unordered_map<std::string, thrift::Value> keyVals;

    for (auto& [key, val] : keyTtlBackoffs) {
      auto& thriftValue = val.first;
      auto& backoff = val.second;
      if (not backoff.canTryNow()) {
        XLOG(DBG2) << "Skipping key: " << key << ", area: " << area.t;
        timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());
        continue;
      }

      // Apply backoff
      backoff.reportError();
      timeout = std::min(timeout, backoff.getTimeRemainingUntilRetry());

      // bump ttl version
      (*thriftValue.ttlVersion_ref())++;
      // Set in keyVals which is going to be advertised to the kvStore.
      DCHECK(not thriftValue.value_ref());
      printKeyValInArea(
          1 /*logLevel*/,
          "Advertising ttl update",
          fmt::format("[Area: {}] ", area.t),
          key,
          thriftValue);
      keyVals.emplace(key, thriftValue);
    }

    // Advertise to KvStore
    if (not keyVals.empty()) {
      setKeysHelper(area, std::move(keyVals));
    }
  }

  // Schedule next-timeout for processing/clearing backoffs
  XLOG(DBG2) << "Scheduling ttl timer after " << timeout.count() << "ms.";
  ttlTimer_->scheduleTimeout(timeout);
}

std::optional<folly::Unit>
KvStoreClientInternal::setKeysHelper(
    AreaId const& area,
    std::unordered_map<std::string, thrift::Value> keyVals) {
  // Return if nothing to advertise.
  if (keyVals.empty()) {
    return folly::Unit();
  }

  // Debugging purpose print-out
  for (auto const& [key, thriftValue] : keyVals) {
    printKeyValInArea(
        3 /*logLevel*/,
        "Send update",
        fmt::format("[Area: {}] ", area.t),
        key,
        thriftValue);
  }

  thrift::KeySetParams params;
  *params.keyVals_ref() = std::move(keyVals);

  try {
    kvStore_->semifuture_setKvStoreKeyVals(area, params).get();
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to set key-val from KvStore. Exception: " << ex.what();
    return std::nullopt;
  }
  return folly::Unit();
}

} // namespace openr
