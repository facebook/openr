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
    std::set<std::string> const& selectAreas,
    thrift::KeyDumpParams filter,
    apache::thrift::ServerStreamPublisher<thrift::Publication>&& publisher)
    : selectAreas_(selectAreas),
      filter_(filter),
      publisher_(std::move(publisher)) {
  std::vector<std::string> keyPrefix;

  if (filter.keys_ref().has_value()) {
    keyPrefix = std::move(*filter.keys_ref());
  } else if (filter.prefix_ref().has_value()) {
    folly::split(",", *filter.prefix_ref(), keyPrefix, true);
  }

  keyPrefixFilter_ =
      KvStoreFilters(keyPrefix, std::move(*filter.originatorIds_ref()));
}

/**
 * A publication object (param) can have multiple key value pairs as follows.
 * pub = {"prefix1": value1, "prefix2": value2, "random-key": random-value}
 * The value objects also contain originator-id.
 *
 * A thrift client can specify filters to be applied on prefixes and
 * originator-ids. Logical operator OR or AND is also specified for combining
 * matches on prefix and originator-id.
 *
 * Suppose filter specified is {"prefix.*"} and logical operator is AND. After
 * applying the filter, the filtered publication = {"prefix1": value1,
 * "prefix2": value2} will be returned to the client (i.e., published) on the
 * stream.
 */
void
KvStorePublisher::publish(const thrift::Publication& pub) {
  LOG(INFO) << pub.get_area();
  if (not(selectAreas_.empty() || selectAreas_.count(pub.get_area()))) {
    LOG(INFO) << "Skipping pub not in selectAreas";
    return;
  }
  if ((not filter_.keys_ref().has_value() or (*filter_.keys_ref()).empty()) and
      (not filter_.originatorIds_ref().has_value() or
       (*filter_.originatorIds_ref()).empty()) and
      not*filter_.ignoreTtl_ref() and not*filter_.doNotPublishValue_ref()) {
    // No filtering criteria. Accept all updates as TTL updates are not be
    // to be updated. If we don't optimize here, we will have go through
    // key values of a publication and copy them.
    auto filteredPub = std::make_unique<thrift::Publication>(pub);
    publisher_.next(std::move(*filteredPub));
    return;
  }

  thrift::Publication publication_filtered;
  if (pub.expiredKeys_ref().has_value()) {
    publication_filtered.expiredKeys_ref() = *pub.expiredKeys_ref();
  }

  if (pub.nodeIds_ref().has_value()) {
    publication_filtered.nodeIds_ref() = *pub.nodeIds_ref();
  }

  if (pub.tobeUpdatedKeys_ref().has_value()) {
    publication_filtered.tobeUpdatedKeys_ref() = *pub.tobeUpdatedKeys_ref();
  }

  if (pub.floodRootId_ref().has_value()) {
    publication_filtered.floodRootId_ref() = *pub.floodRootId_ref();
  }

  if (pub.area_ref().has_value()) {
    publication_filtered.area_ref() = *pub.area_ref();
  }

  thrift::KeyVals keyvals;
  thrift::FilterOperator op = filter_.oper_ref().has_value()
      ? *filter_.oper_ref()
      : thrift::FilterOperator::OR;

  for (auto& kv : *pub.keyVals_ref()) {
    auto& key = kv.first;
    auto& val = kv.second;
    if (*filter_.ignoreTtl_ref() and not val.value_ref().has_value()) {
      // ignore TTL updates
      continue;
    }

    if (not keyPrefixFilter_.keyMatch(key, val, op)) {
      continue;
    }

    if ((not*filter_.doNotPublishValue_ref()) or
        (not val.value_ref().has_value())) {
      keyvals.emplace(key, val);
    } else {
      // Exclude Value.value if it's filtered
      keyvals.emplace(key, createThriftValueWithoutBinaryValue(val));
    }
  }

  if (keyvals.size()) {
    // There is at least one key value in the publication for the client
    publication_filtered.keyVals_ref() = keyvals;
    publisher_.next(std::move(publication_filtered));
  }
}
} // namespace openr
