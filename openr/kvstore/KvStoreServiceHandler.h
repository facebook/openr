/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb303/BaseService.h>
#include <openr/if/gen-cpp2/KvStoreService.h>
#include <openr/if/gen-cpp2/KvStoreServiceAsyncClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

class KvStoreServiceHandler final : public thrift::KvStoreServiceSvIf,
                                    public facebook::fb303::BaseService {
 public:
  KvStoreServiceHandler(
      const std::string& nodeName,
      KvStore<thrift::KvStoreServiceAsyncClient>* kvStore);

  /*
   * util function to return node name
   */
  inline const std::string&
  getNodeName() {
    return nodeName_;
  }

  /*
   * API to return key-val pairs by given:
   *  - a set of string "keys"; (ATTN: this is NOT regex matching)
   *  - a specific area;
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsArea(
      std::unique_ptr<std::vector<std::string>> filterKeys,
      std::unique_ptr<std::string> area) override;

  /*
   * API to return key-val pairs by given:
   *  - thrift::KeyDumpParams;
   *  - a specific area;
   *
   * ATTN: thrift::KeyDumpParams is an advanced version of matching with:
   *  - keyPrefixList; (ATTN: this is regex matching)
   *  - originatorIds; (ATTN: this is NOT regex matching)
   *  - operator to support AND/OR condition matching;
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreKeyValsFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  /*
   * API to return key-val HASHes(NO binary value included) only by given:
   *  - thrift::KeyDumpParams;
   *  - a specific area;
   *
   * ATTN: same as above usage of thrift::KeyDumpParams
   */
  folly::SemiFuture<std::unique_ptr<thrift::Publication>>
  semifuture_getKvStoreHashFilteredArea(
      std::unique_ptr<thrift::KeyDumpParams> filter,
      std::unique_ptr<std::string> area) override;

  /*
   * API to set key-val pairs by given:
   *  - thrift::KeySetParams;
   *  - a specifc area;
   *
   * ATTN: set key will automatically trigger (K, V) merge operation to:
   *  1. update local kvStoreDb;
   *  2. flood the delta update to peers if any;
   */
  folly::SemiFuture<folly::Unit> semifuture_setKvStoreKeyVals(
      std::unique_ptr<thrift::KeySetParams> setParams,
      std::unique_ptr<std::string> area) override;

  /*
   * API to dump existing peers in a specified area
   */
  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeersArea(std::unique_ptr<std::string> area) override;

  /*
   * API to return structured thrift::KvStoreAreaSummary to include:
   *  - selected area names;
   *  - number of key-val pairs;
   *  - total of key-val bytes;
   *  - peers in each area;
   *  - counters in each area;
   *  - etc;
   */
  folly::SemiFuture<std::unique_ptr<::std::vector<thrift::KvStoreAreaSummary>>>
  semifuture_getKvStoreAreaSummary(
      std::unique_ptr<std::set<std::string>> selectAreas) override;

 private:
  const std::string nodeName_;
  KvStore<thrift::KvStoreServiceAsyncClient>* kvStore_{nullptr};

}; // class KvStoreServiceHandler

} // namespace openr
