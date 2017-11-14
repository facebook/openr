/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PersistentStoreClient.h"

namespace openr {

PersistentStoreClient::PersistentStoreClient(
    const PersistentStoreUrl& socketUrl, fbzmq::Context& context)
    : reqSocket_(context) {
  VLOG(3) << "PersistentStoreClient: Connecting to server socket.";
  reqSocket_.connect(fbzmq::SocketUrl{socketUrl});
}

folly::Expected<bool, fbzmq::Error>
PersistentStoreClient::erase(std::string const& key) noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE,
      thrift::StoreRequestType::ERASE,
      key,
      "" /* data */);
  reqSocket_.sendThriftObj(request, serializer_);

  auto response = reqSocket_.recvThriftObj<thrift::StoreResponse>(serializer_);
  if (response.hasValue()) {
    CHECK_EQ(key, response->key);
    return response->success;
  }

  return folly::makeUnexpected(response.error());
}

folly::Expected<bool, fbzmq::Error>
PersistentStoreClient::storeInternal(
    std::string const& key, std::string const& data) const noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE, thrift::StoreRequestType::STORE, key, data);
  reqSocket_.sendThriftObj(request, serializer_);

  auto response = reqSocket_.recvThriftObj<thrift::StoreResponse>(serializer_);
  if (response.hasValue()) {
    return response->success;
  }
  return folly::makeUnexpected(response.error());
}

folly::Expected<std::string, fbzmq::Error>
PersistentStoreClient::loadInternal(std::string const& key) const noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE,
      thrift::StoreRequestType::LOAD,
      key,
      "" /* empty data */);
  reqSocket_.sendThriftObj(request, serializer_);

  auto response = reqSocket_.recvThriftObj<thrift::StoreResponse>(serializer_);
  if (response.hasValue()) {
    CHECK_EQ(key, response->key);
    if (not response->success) {
      return folly::makeUnexpected(fbzmq::Error(0, "key not found"));
    } else {
      return response->data;
    }
  }
  return folly::makeUnexpected(response.error());
}

} // namespace openr
