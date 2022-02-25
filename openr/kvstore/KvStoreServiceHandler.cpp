/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/kvstore/KvStoreServiceHandler.h>

namespace fb303 = facebook::fb303;

namespace openr {

KvStoreServiceHandler::KvStoreServiceHandler(
    const std::string& nodeName,
    KvStore<thrift::KvStoreServiceAsyncClient>* kvStore)
    : fb303::BaseService("kvstore"), nodeName_(nodeName), kvStore_(kvStore) {
  CHECK_NOTNULL(kvStore_);
}

//
// KvStore APIs
//

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler::semifuture_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  thrift::KeyGetParams params;
  params.keys_ref() = std::move(*filterKeys);

  return kvStore_->semifuture_getKvStoreKeyVals(
      std::move(*area), std::move(params));
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler::semifuture_getKvStoreKeyValsFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_dumpKvStoreKeys(std::move(*filter), {*area})
      .deferValue(
          [](std::unique_ptr<std::vector<thrift::Publication>>&& pubs) mutable {
            thrift::Publication pub = pubs->empty() ? thrift::Publication{}
                                                    : std::move(*pubs->begin());
            return std::make_unique<thrift::Publication>(std::move(pub));
          });
}

folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler::semifuture_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_dumpKvStoreHashes(
      std::move(*area), std::move(*filter));
}

folly::SemiFuture<folly::Unit>
KvStoreServiceHandler::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_setKvStoreKeyVals(
      std::move(*area), std::move(*setParams));
}

folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
KvStoreServiceHandler::semifuture_getKvStorePeersArea(
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_getKvStorePeers(std::move(*area));
}

folly::SemiFuture<std::unique_ptr<::std::vector<thrift::KvStoreAreaSummary>>>
KvStoreServiceHandler::semifuture_getKvStoreAreaSummary(
    std::unique_ptr<std::set<std::string>> selectAreas) {
  return kvStore_->semifuture_getKvStoreAreaSummaryInternal(
      std::move(*selectAreas));
}

} // namespace openr
