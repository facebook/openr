/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <folly/futures/Future.h>
#include <openr/if/gen-cpp2/KvStoreService.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

/*
 * FakeKvStoreHandler implements a minimal KvStore Thrift service for scale
 * testing.
 *
 * This handler represents one fake neighbor's KV store. When the DUT's
 * KvStore tries to sync with this neighbor, it calls:
 *   1. getKvStoreKeyValsFilteredArea() - DUT sends hashes, we return diff
 *   2. setKvStoreKeyVals() - DUT sends keys we requested (we just accept)
 *
 * The handler maintains a copy of the KV data for this neighbor, which can
 * be updated to simulate topology changes propagated via flooding.
 *
 * Thread safety: All methods are thread-safe via internal mutex.
 */
class FakeKvStoreHandler : public thrift::KvStoreServiceSvIf {
 public:
  /*
   * Construct a handler for a specific neighbor.
   *
   * @param neighborName The name of this fake neighbor
   * @param kvStore Initial KV data for this neighbor's store
   */
  FakeKvStoreHandler(std::string neighborName, thrift::KeyVals kvStore);

  /*
   * Construct a handler sharing immutable KV data (COW path).
   *
   * Multiple handlers can share the same underlying data. On first write,
   * the handler materializes a private copy (copy-on-write).
   *
   * @param neighborName The name of this fake neighbor
   * @param sharedKvStore Shared immutable KV data
   */
  FakeKvStoreHandler(
      std::string neighborName,
      std::shared_ptr<const thrift::KeyVals> sharedKvStore);

  /*
   * 3-way sync step 1+2: DUT sends hashes, we return diff.
   *
   * The filter contains keyValHashes from the DUT. We compare against our
   * local store and return:
   *   - keyVals: keys we have that DUT doesn't, or have newer versions
   *   - tobeUpdatedKeys: keys DUT has that we want (DUT will send via
   * setKvStoreKeyVals)
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  /*
   * 3-way sync step 3: DUT sends back keys we requested.
   *
   * In a real KvStore, we'd merge these into our local store.
   * For fake neighbors, we just accept and log.
   */
  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> setParams,
      std::unique_ptr<std::string> area) override;

  /*
   * Return specific keys by exact name (not regex).
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsArea(
      std::unique_ptr<std::vector<std::string>> filterKeys,
      std::unique_ptr<std::string> area) override;

  /*
   * Return hashes only (no binary values).
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreHashFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  /*
   * Full replacement of KV data.
   */
  void updateKvStore(thrift::KeyVals newKvStore);

  /*
   * Update/insert a single key — used by topology change propagation.
   */
  void updateKey(const std::string& key, thrift::Value value);

  /*
   * Remove a single key.
   */
  void removeKey(const std::string& key);

  /*
   * Reset to shared immutable KV data (COW).
   *
   * Discards any local mutations and reverts to sharing the given data.
   * Used by updateTopology to avoid per-handler deep copies.
   */
  void resetToShared(std::shared_ptr<const thrift::KeyVals> sharedKvStore);

  /*
   * Get the neighbor name this handler represents.
   */
  const std::string&
  getNeighborName() const {
    return neighborName_;
  }

  /*
   * Get a copy of the current KV store (for debugging/testing).
   */
  thrift::KeyVals getKvStore() const;

 private:
  /*
   * Return a const reference to the active KV store.
   * Must be called under mutex_.
   */
  const thrift::KeyVals& store() const;

  /*
   * Materialize a private copy if still sharing, then return mutable ref.
   * Must be called under mutex_.
   */
  thrift::KeyVals& mutableStore();

  std::string neighborName_;

  // COW storage: at most one of these is active at a time.
  // sharedStore_ holds the immutable shared base (COW path).
  // ownedStore_ holds a private mutable copy (materialized on first write).
  std::shared_ptr<const thrift::KeyVals> sharedStore_;
  std::optional<thrift::KeyVals> ownedStore_;

  mutable std::mutex mutex_;
};

} // namespace openr
