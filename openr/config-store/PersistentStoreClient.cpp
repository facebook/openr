/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PersistentStoreClient.h"

#include <openr/common/Constants.h>

namespace openr {

PersistentStoreClient::PersistentStoreClient(
    const PersistentStoreUrl& socketUrl, fbzmq::Context& context)
    : context_(context),
      reqSocketUrl_(socketUrl) {}

folly::Expected<bool, fbzmq::Error>
PersistentStoreClient::erase(std::string const& key) noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE,
      thrift::StoreRequestType::ERASE,
      key,
      "" /* data */);

  auto response = requestReply(request);
  if (response.hasValue()) {
    CHECK_EQ(key, response->key);
    return response->success;
  }

  return folly::makeUnexpected(response.error());
}

folly::Expected<bool, fbzmq::Error>
PersistentStoreClient::storeInternal(
    std::string const& key, std::string const& data) noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE, thrift::StoreRequestType::STORE, key, data);

  auto response = requestReply(request);
  if (response.hasValue()) {
    return response->success;
  }
  return folly::makeUnexpected(response.error());
}

folly::Expected<std::string, fbzmq::Error>
PersistentStoreClient::loadInternal(std::string const& key) noexcept {
  thrift::StoreRequest request(
      apache::thrift::FRAGILE,
      thrift::StoreRequestType::LOAD,
      key,
      "" /* empty data */);

  auto response = requestReply(request);
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

folly::Expected<thrift::StoreResponse, fbzmq::Error>
PersistentStoreClient::requestReply(
    thrift::StoreRequest const& request) noexcept {
  // Create request socket if not yet created!
  if (!reqSocket_) {
    reqSocket_ = std::make_unique<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>>(
        context_, folly::none, folly::none, fbzmq::NonblockingFlag{false});
    VLOG(3) << "PersistentStoreClient: Connecting to server socket.";
    auto connectRet = reqSocket_->connect(fbzmq::SocketUrl{reqSocketUrl_});
    if (connectRet.hasError()) {
      LOG(FATAL) << "Failed to connect to server. " << connectRet.error();
    }
  }

  CHECK(reqSocket_);

  // Send request
  auto reqRet = reqSocket_->sendThriftObj(request, serializer_);
  if (reqRet.hasError()) {
    reqSocket_.reset();
    return folly::makeUnexpected(reqRet.error());
  }

  // Receive response
  auto resRet = reqSocket_->recvThriftObj<thrift::StoreResponse>(
      serializer_, Constants::kReadTimeout);
  if (resRet.hasError()) {
    reqSocket_.reset();
    return folly::makeUnexpected(resRet.error());
  }

  return resRet.value();
}

} // namespace openr
