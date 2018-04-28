/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Expected.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/config-store/PersistentStore.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>

namespace openr {

class PersistentStoreClient {
 public:
  PersistentStoreClient(
      const PersistentStoreUrl& socketUrl, fbzmq::Context& context);

  //
  // Load/Store thrift object types
  //

  template <typename ThriftType>
  folly::Expected<bool, fbzmq::Error>
  storeThriftObj(std::string const& key, ThriftType const& value)
      noexcept {
    auto msg = fbzmq::Message::fromThriftObj(value, serializer_);
    return storeInternal(
        key,
        std::string(
            reinterpret_cast<const char*>(msg->data().data()), msg->size()));
  }

  template <typename ThriftType>
  folly::Expected<ThriftType, fbzmq::Error>
  loadThriftObj(std::string const& key) noexcept {
    auto data = loadInternal(key);
    if (data.hasValue()) {
      auto thriftObj = fbzmq::Message::from(*data);
      return thriftObj->readThriftObj<ThriftType>(serializer_);
    }
    return folly::makeUnexpected(data.error());
  }

  //
  // Load/Store fundamental types (integer, string etc.)
  //

  template <
      typename T,
      std::enable_if_t<std::is_fundamental<std::decay_t<T>>::value>* = nullptr>
  folly::Expected<bool, fbzmq::Error>
  store(std::string const& key, T const& value) noexcept {
    return storeInternal(
        key, std::string(reinterpret_cast<const char*>(&value), sizeof(value)));
  }
  folly::Expected<bool, fbzmq::Error>
  store(std::string const& key, std::string const& value) noexcept {
    return storeInternal(key, value);
  }

  template <
      typename T,
      std::enable_if_t<std::is_fundamental<std::decay_t<T>>::value>* = nullptr>
  folly::Expected<T, fbzmq::Error>
  load(std::string const& key) noexcept {
    auto data = loadInternal(key);
    if (data.hasError()) {
      return folly::makeUnexpected(data.error());
    }

    if (sizeof(T) != data->size()) {
      return folly::makeUnexpected(fbzmq::Error(0, "malformed data"));
    }

    T t;
    memcpy(reinterpret_cast<void*>(&t), data->data(), data->size());
    return t;
  }
  template <
      typename T,
      std::enable_if_t<std::is_same<std::decay_t<T>, std::string>::value>* =
          nullptr>
  folly::Expected<std::string, fbzmq::Error>
  load(std::string const& key) noexcept {
    return loadInternal(key);
  }

  //
  // API to `Erase` key from the config store. Return value indicates if key-was
  // found or not (like `std::map::erase`)
  //
  folly::Expected<bool, fbzmq::Error> erase(std::string const& key) noexcept;

 private:
  //
  // Internal implementation of load/store
  //
  folly::Expected<bool, fbzmq::Error> storeInternal(
      std::string const& key, std::string const& data) noexcept;
  folly::Expected<std::string, fbzmq::Error> loadInternal(
      std::string const& key) noexcept;

  // Internal method to create socket lazily and perform send/recv
  folly::Expected<thrift::StoreResponse, fbzmq::Error> requestReply(
      thrift::StoreRequest const& request) noexcept;

  // Socket for communicating with storage module
  fbzmq::Context& context_;
  const PersistentStoreUrl reqSocketUrl_;
  std::unique_ptr<fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>> reqSocket_;

  // Thrift CompactSerializer for send/recv of thrift objects over reqSocket_
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
