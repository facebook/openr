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
    const std::string& nodeName,
    KvStore<ClientType>* kvStore,
    std::shared_ptr<OpenrIngressKvStoreBackpressure>
        openrIngressKvStoreBackpressure)
    : fb303::BaseService("kvstore"),
      nodeName_(nodeName),
      kvStore_(kvStore),
      openrIngressKvStoreBackpressure_(
          std::move(openrIngressKvStoreBackpressure)) {
  CHECK_NOTNULL(kvStore_);
}

template <class ClientType>
size_t
KvStoreServiceHandler<ClientType>::recordPeerAndGetRequestWireBytes(
    const thrift::KeySetParams& setParams) {
  auto* requestContext = getRequestContext();
  if (!requestContext) {
    return 0;
  }
  auto* connection = requestContext->getConnectionContext();
  if (openrIngressKvStoreBackpressure_ && connection) {
    openrIngressKvStoreBackpressure_->recordSenderId(
        *connection, setParams.senderId().value_or(""));
  }
  return requestContext->getWiredRequestBytes();
}

template <class ClientType>
std::unique_ptr<
    OpenrIngressKvStoreBackpressure::OpenrIngressKvStorePendingMergeGuard>
KvStoreServiceHandler<ClientType>::trackOpenrIngressKvStorePendingMergeBytes(
    const thrift::KeySetParams& setParams) {
  if (!openrIngressKvStoreBackpressure_) {
    return nullptr;
  }
  return openrIngressKvStoreBackpressure_
      ->trackOpenrIngressKvStorePendingMergeBytes(
          setParams.senderId().value_or(""),
          recordPeerAndGetRequestWireBytes(setParams));
}

/*
 *
 * KvStore APIs
 *
 */

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler<ClientType>::co_getKvStoreKeyValsArea(
    std::unique_ptr<std::vector<std::string>> filterKeys,
    std::unique_ptr<std::string> area) {
  thrift::KeyGetParams params;
  params.keys() = std::move(*filterKeys);

  co_return co_await kvStore_->co_getKvStoreKeyVals(
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
folly::coro::Task<std::unique_ptr<thrift::Publication>>
KvStoreServiceHandler<ClientType>::co_getKvStoreHashFilteredArea(
    std::unique_ptr<thrift::KeyDumpParams> filter,
    std::unique_ptr<std::string> area) {
  co_return co_await kvStore_->co_dumpKvStoreHashes(
      std::move(*area), std::move(*filter));
}

template <class ClientType>
folly::coro::Task<void>
KvStoreServiceHandler<ClientType>::co_setKvStoreKeyVals(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  [[maybe_unused]] const auto openrIngressKvStorePendingMergeGuard =
      trackOpenrIngressKvStorePendingMergeBytes(*setParams);
  co_await kvStore_->co_setKvStoreKeyVals(
      std::move(*area), std::move(*setParams));
  co_return;
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::SetKeyValsResult>>
KvStoreServiceHandler<ClientType>::co_setKvStoreKeyValues(
    std::unique_ptr<thrift::KeySetParams> setParams,
    std::unique_ptr<std::string> area) {
  // Keeps this request's bytes counted until the merge below finishes.
  [[maybe_unused]] const auto openrIngressKvStorePendingMergeGuard =
      trackOpenrIngressKvStorePendingMergeBytes(*setParams);
  co_return co_await kvStore_->co_setKvStoreKeyValues(
      std::move(*area), std::move(*setParams));
}

template <class ClientType>
folly::coro::Task<std::unique_ptr<thrift::PeersMap>>
KvStoreServiceHandler<ClientType>::co_getKvStorePeersArea(
    std::unique_ptr<std::string> area) {
  co_return co_await kvStore_->co_getKvStorePeers(std::move(*area));
}

} // namespace openr
