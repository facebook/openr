/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/kvstore/KvStorePublisher.h>

#include <re2/re2.h>

#include <folly/ExceptionString.h>
#include <folly/io/async/SSLContext.h>
#include <folly/io/async/ssl/OpenSSLUtils.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/PersistentStore_types.h>
#include <openr/kvstore/KvStore.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

namespace openr {

KvStorePublisher::KvStorePublisher(
    thrift::KvFilter filter,
    apache::thrift::ServerStreamPublisher<thrift::Publication>&& publisher)
    : filter_(filter), publisher_(std::move(publisher)) {
  std::vector<std::string> keyPrefix;
  std::set<std::string> originatorIds;

  if (filter.keys_ref().has_value()) {
    keyPrefix = std::move(*filter.keys_ref());
  }

  if (filter.originatorIds_ref().has_value()) {
    originatorIds = std::move(*filter.originatorIds_ref());
  }

  keyPrefixFilter_ = KvStoreFilters(keyPrefix, originatorIds);
}

void
KvStorePublisher::publish(const thrift::Publication& pub) {
  if (!filter_.keys_ref() && !filter_.originatorIds_ref()) {
    auto publication = std::make_unique<thrift::Publication>(pub);
    publisher_.next(std::move(*publication));
    return;
  }

  for (auto& kv : pub.keyVals) {
    auto& key = kv.first;
    auto& val = kv.second;
    if (!val.value_ref().has_value()) {
      continue;
    }

    if (keyPrefixFilter_.keyMatch(key, val)) {
      auto publication = std::make_unique<thrift::Publication>(pub);
      publisher_.next(std::move(*publication));
      return;
    }
  }
}
} // namespace openr
