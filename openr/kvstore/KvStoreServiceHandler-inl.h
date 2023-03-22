/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef KVSTORE_SERVICE_HANDLER_H_
#error This file may only be included from KvStoreServiceHandler.h
#endif

namespace fb303 = facebook::fb303;

namespace openr {

template <class ClientType>
KvStoreServiceHandler<ClientType>::KvStoreServiceHandler(
    const std::string& nodeName, KvStore<ClientType>* kvStore)
    : fb303::BaseService("kvstore"), nodeName_(nodeName), kvStore_(kvStore) {
  CHECK_NOTNULL(kvStore_);
}

//
// KvStore APIs
//

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler<ClientType>::semifuture_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  thrift::KeyGetParams params;
  params.keys() = std::move(*filterKeys);

  return kvStore_->semifuture_getKvStoreKeyVals(
      std::move(*area), std::move(params));
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler<ClientType>::semifuture_getKvStoreKeyValsFilteredArea(
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

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler<ClientType>::semifuture_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_dumpKvStoreHashes(
      std::move(*area), std::move(*filter));
}

template <class ClientType>
folly::SemiFuture<folly::Unit>
KvStoreServiceHandler<ClientType>::semifuture_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_setKvStoreKeyVals(
      std::move(*area), std::move(*setParams));
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::SetKeyValsResult>>
KvStoreServiceHandler<ClientType>::semifuture_setKvStoreKeyValues(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_setKvStoreKeyValues(
      std::move(*area), std::move(*setParams));
}

template <class ClientType>
folly::SemiFuture<std::unique_ptr<thrift::PeersMap>>
KvStoreServiceHandler<ClientType>::semifuture_getKvStorePeersArea(
    std::unique_ptr<std::string> area) {
  return kvStore_->semifuture_getKvStorePeers(std::move(*area));
}

} // namespace openr
