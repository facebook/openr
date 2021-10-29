/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Types.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/OpenrCtrlCpp.h>
#include <openr/if/gen-cpp2/Types_constants.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <chrono>
#include <cstddef>
#include <optional>

namespace openr {
class KvStorePublisher {
 public:
  KvStorePublisher(
      std::set<std::string> const& selectAreas,
      thrift::KeyDumpParams filter,
      apache::thrift::ServerStreamPublisher<thrift::Publication>&& publisher,
      std::chrono::steady_clock::time_point subscription_time =
          std::chrono::steady_clock::now(),
      int64_t total_messages = 0);

  ~KvStorePublisher() {}

  // Invoked whenever there is change. Apply filter and publish changes
  void publish(const thrift::Publication& pub);

  template <class... Args>
  void
  complete(Args&&... args) {
    std::move(publisher_).complete(std::forward<Args>(args)...);
  }

 private:
  // set of areas whose updates should be published. If empty, publish all
  std::set<std::string> selectAreas_;
  thrift::KeyDumpParams filter_;
  KvStoreFilters keyPrefixFilter_{{}, {}};
  apache::thrift::ServerStreamPublisher<thrift::Publication> publisher_;

 public:
  std::chrono::steady_clock::time_point subscription_time_;
  int64_t total_messages_;
  std::chrono::system_clock::time_point last_message_time_;
};
} // namespace openr
