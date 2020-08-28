/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>
#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/KvStore_constants.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/OpenrCtrlCpp.h>
#include <openr/kvstore/KvStore.h>

namespace openr {
class KvStorePublisher {
 public:
  KvStorePublisher(
      thrift::KeyDumpParams filter,
      apache::thrift::ServerStreamPublisher<thrift::Publication>&& publisher);

  ~KvStorePublisher() {}

  // Invoked whenever there is change. Apply filter and publish changes
  void publish(const thrift::Publication& pub);

  void
  complete() {
    std::move(publisher_).complete();
  }

 private:
  thrift::KeyDumpParams filter_;
  KvStoreFilters keyPrefixFilter_{{}, {}};
  apache::thrift::ServerStreamPublisher<thrift::Publication> publisher_;
};
} // namespace openr
