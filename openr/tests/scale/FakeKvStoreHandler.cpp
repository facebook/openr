/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/FakeKvStoreHandler.h>

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include <openr/kvstore/KvStoreUtil.h>

namespace openr {

FakeKvStoreHandler::FakeKvStoreHandler(
    std::string neighborName, thrift::KeyVals kvStore)
    : neighborName_(std::move(neighborName)), ownedStore_(std::move(kvStore)) {
  XLOGF(
      INFO,
      "[FAKE-KVSTORE] Handler created for neighbor '{}' with {} keys",
      neighborName_,
      ownedStore_->size());
}

FakeKvStoreHandler::FakeKvStoreHandler(
    std::string neighborName,
    std::shared_ptr<const thrift::KeyVals> sharedKvStore)
    : neighborName_(std::move(neighborName)),
      sharedStore_(std::move(sharedKvStore)) {
  XLOGF(
      INFO,
      "[FAKE-KVSTORE] Handler created for neighbor '{}' with {} keys (shared/COW)",
      neighborName_,
      sharedStore_->size());
}

const thrift::KeyVals&
FakeKvStoreHandler::store() const {
  if (ownedStore_.has_value()) {
    return *ownedStore_;
  }
  return *sharedStore_;
}

thrift::KeyVals&
FakeKvStoreHandler::mutableStore() {
  if (!ownedStore_.has_value()) {
    ownedStore_ = *sharedStore_;
    sharedStore_.reset();
  }
  return *ownedStore_;
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
FakeKvStoreHandler::semifuture_getKvStoreKeyValsFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto pub = std::make_unique<thrift::Publication>();

  if (filter->keyValHashes().has_value()) {
    /*
     * 3-way sync: DUT sent us hashes, we compute the diff.
     * dumpDifference returns:
     *   - keyVals: keys we have that DUT doesn't, or have newer versions
     *   - tobeUpdatedKeys: keys DUT has that we want
     */
    *pub = dumpDifference(*area, store(), filter->keyValHashes().value());
    XLOGF(
        DBG2,
        "[FAKE-KVSTORE] {} getKvStoreKeyValsFilteredArea: "
        "area={}, DUT sent {} hashes, returning {} keyVals, {} tobeUpdatedKeys",
        neighborName_,
        *area,
        filter->keyValHashes()->size(),
        pub->keyVals()->size(),
        pub->tobeUpdatedKeys()->size());
  } else {
    /*
     * No hashes provided — return all keys matching the filter.
     * For simplicity, we return all keys (the filter is typically empty
     * or matches everything during initial sync).
     */
    pub->keyVals() = store();
    pub->area() = *area;
    XLOGF(
        DBG2,
        "[FAKE-KVSTORE] {} getKvStoreKeyValsFilteredArea: "
        "area={}, no hashes, returning all {} keys",
        neighborName_,
        *area,
        store().size());
  }

  return folly::makeSemiFuture(std::move(pub));
}

folly::SemiFuture<folly::Unit>
FakeKvStoreHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  /*
   * 3-way sync step 3: DUT sends back keys we requested.
   * In a real KvStore, we'd merge these. For fake neighbors, we just log
   * and optionally merge to keep our store in sync with DUT.
   */
  std::lock_guard<std::mutex> lock(mutex_);

  size_t numKeys = setParams->keyVals()->size();
  XLOGF(
      DBG2,
      "[FAKE-KVSTORE] {} setKvStoreKeyVals: area={}, received {} keys from DUT",
      neighborName_,
      *area,
      numKeys);

  /*
   * Merge incoming keys into our store (optional, but keeps stores in sync).
   */
  for (auto& [key, value] : *setParams->keyVals()) {
    auto it = mutableStore().find(key);
    if (it == mutableStore().end()) {
      mutableStore().emplace(key, std::move(value));
    } else {
      /*
       * Simple version comparison — higher version wins.
       */
      if (*value.version() > *it->second.version()) {
        it->second = std::move(value);
      }
    }
  }

  return folly::makeSemiFuture(folly::Unit{});
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
FakeKvStoreHandler::semifuture_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto pub = std::make_unique<thrift::Publication>();
  pub->area() = *area;

  for (const auto& key : *filterKeys) {
    auto it = store().find(key);
    if (it != store().end()) {
      pub->keyVals()->emplace(it->first, it->second);
    }
  }

  XLOGF(
      DBG2,
      "[FAKE-KVSTORE] {} getKvStoreKeyValsArea: "
      "area={}, requested {} keys, found {}",
      neighborName_,
      *area,
      filterKeys->size(),
      pub->keyVals()->size());

  return folly::makeSemiFuture(std::move(pub));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
FakeKvStoreHandler::semifuture_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> /* filter */,
    std::unique_ptr<std::string> area) {
  std::lock_guard<std::mutex> lock(mutex_);

  /*
   * Return hashes only (no binary values).
   * This is used during sync to compare what keys exist.
   */
  auto pub = std::make_unique<thrift::Publication>();
  pub->area() = *area;

  for (const auto& [key, value] : store()) {
    thrift::Value hashOnly;
    hashOnly.version() = *value.version();
    hashOnly.originatorId() = *value.originatorId();
    hashOnly.ttl() = *value.ttl();
    hashOnly.ttlVersion() = *value.ttlVersion();
    hashOnly.hash() = value.hash().value_or(0);
    pub->keyVals()->emplace(key, std::move(hashOnly));
  }

  XLOGF(
      DBG2,
      "[FAKE-KVSTORE] {} getKvStoreHashFilteredArea: "
      "area={}, returning {} hashes",
      neighborName_,
      *area,
      pub->keyVals()->size());

  return folly::makeSemiFuture(std::move(pub));
}

void
FakeKvStoreHandler::updateKvStore(thrift::KeyVals newKvStore) {
  std::lock_guard<std::mutex> lock(mutex_);
  ownedStore_ = std::move(newKvStore);
  sharedStore_.reset();
  XLOGF(
      DBG1,
      "[FAKE-KVSTORE] {} KV store replaced with {} keys",
      neighborName_,
      ownedStore_->size());
}

void
FakeKvStoreHandler::updateKey(const std::string& key, thrift::Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  mutableStore()[key] = std::move(value);
  XLOGF(
      DBG2,
      "[FAKE-KVSTORE] {} key '{}' updated (version={})",
      neighborName_,
      key,
      *mutableStore()[key].version());
}

void
FakeKvStoreHandler::removeKey(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto erased = mutableStore().erase(key);
  if (erased > 0) {
    XLOGF(DBG2, "[FAKE-KVSTORE] {} key '{}' removed", neighborName_, key);
  }
}

thrift::KeyVals
FakeKvStoreHandler::getKvStore() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return store();
}

void
FakeKvStoreHandler::resetToShared(
    std::shared_ptr<const thrift::KeyVals> sharedKvStore) {
  std::lock_guard<std::mutex> lock(mutex_);
  sharedStore_ = std::move(sharedKvStore);
  ownedStore_.reset();
  XLOGF(
      DBG1,
      "[FAKE-KVSTORE] {} KV store reset to shared ({} keys)",
      neighborName_,
      sharedStore_->size());
}

} // namespace openr
