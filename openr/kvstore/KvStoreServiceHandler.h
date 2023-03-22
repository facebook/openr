/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fb303/BaseService.h>
#include <openr/if/gen-cpp2/KvStoreService.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/kvstore/KvStore.h>

namespace openr {

/*
 * This is the template class to handle synchronization between multiple
 * KvStore instances. KvStore does initial FULL_SYNC and INCREMENTAL_FLOOD
 * via thrift channel I/O. Sample ClientType can be:
 *
 *  1) thrift::KvStoreServiceAsyncClient - this is the general ClientType
 *  2) thrift::OpenrCtrlCppAsyncClient - this is the routing-protocol specific
 *     ClientType
 */
template <class ClientType>
class KvStoreServiceHandler final : public thrift::KvStoreServiceSvIf,
                                    public facebook::fb303::BaseService {
 public:
  KvStoreServiceHandler(
      const std::string& nodeName, KvStore<ClientType>* kvStore);

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
   * API to set key-val pairs by given:
   *  - thrift::KeySetParams;
   *  - a specifc area;
   *
   * ATTN: set key will automatically trigger (K, V) merge operation to:
   *  1. update local kvStoreDb;
   *  2. flood the delta update to peers if any;
   */
  folly::SemiFuture<std::unique_ptr<thrift::SetKeyValsResult>>
  semifuture_setKvStoreKeyValues(
      std::unique_ptr<thrift::KeySetParams> setParams,
      std::unique_ptr<std::string> area) override;

  /*
   * API to dump existing peers in a specified area
   */
  folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
  semifuture_getKvStorePeersArea(std::unique_ptr<std::string> area) override;

 private:
  const std::string nodeName_;
  KvStore<ClientType>* kvStore_{nullptr};

}; // class KvStoreServiceHandler

} // namespace openr

#define KVSTORE_SERVICE_HANDLER_H_
#include <openr/kvstore/KvStoreServiceHandler-inl.h>
#undef KVSTORE_SERVICE_HANDLER_H_
