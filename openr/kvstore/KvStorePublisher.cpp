/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Util.h>
#include <openr/kvstore/KvStorePublisher.h>

namespace openr {

KvStorePublisher::KvStorePublisher(
    std::set<std::string> const& selectAreas,
    thrift::KeyDumpParams filter,
    apache::thrift::ServerStreamPublisher<thrift::Publication>&& publisher,
    std::chrono::steady_clock::time_point subscription_time,
    int64_t total_messages)
    : selectAreas_(selectAreas),
      filter_(filter),
      publisher_(std::move(publisher)),
      subscription_time_(subscription_time),
      total_messages_(total_messages) {
  thrift::FilterOperator op =
      filter_.oper().has_value() ? *filter_.oper() : thrift::FilterOperator::OR;

  keyPrefixFilter_ =
      KvStoreFilters(*filter.keys(), std::move(*filter.originatorIds()), op);
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
  if (!(selectAreas_.empty() || selectAreas_.count(*pub.area()))) {
    return;
  }
  if ((!filter_.keys().has_value() || (*filter_.keys()).empty()) &&
      (!filter_.originatorIds().is_set() ||
       (*filter_.originatorIds()).empty()) &&
      !*filter_.ignoreTtl() && !*filter_.doNotPublishValue()) {
    // No filtering criteria. Accept all updates as TTL updates are not be
    // to be updated. If we don't optimize here, we will have go through
    // key values of a publication and copy them.
    auto filteredPub = std::make_unique<thrift::Publication>(pub);
    filteredPub->timestamp_ms() = getUnixTimeStampMs();
    publisher_.next(std::move(*filteredPub));
    return;
  }

  thrift::Publication publication_filtered;
  publication_filtered.area() = *pub.area();
  publication_filtered.keyVals() = getFilteredKeyVals(*pub.keyVals());
  publication_filtered.expiredKeys() =
      getFilteredExpiredKeys(*pub.expiredKeys());

  if (pub.nodeIds()) {
    publication_filtered.nodeIds() = *pub.nodeIds();
  }

  if (pub.tobeUpdatedKeys()) {
    publication_filtered.tobeUpdatedKeys() = *pub.tobeUpdatedKeys();
  }

  if (publication_filtered.keyVals()->size() ||
      publication_filtered.expiredKeys()->size()) {
    // There is at least one key value in the publication for the client
    // or there are some expiredKeys
    publication_filtered.timestamp_ms() = getUnixTimeStampMs();
    publisher_.next(std::move(publication_filtered));
  }
}

thrift::KeyVals
KvStorePublisher::getFilteredKeyVals(const thrift::KeyVals& origKeyVals) {
  // The value field may be explicitly excluded/ignored by the doNotPublishValue
  // flag
  thrift::KeyVals keyvals;
  for (auto& [key, val] : origKeyVals) {
    if (*filter_.ignoreTtl() && !val.value().has_value()) {
      // ignore TTL updates
      continue;
    }

    if (!keyPrefixFilter_.keyMatch(key, val)) {
      continue;
    }

    if (*filter_.doNotPublishValue() && val.value().has_value()) {
      // Exclude Value.value if it's filtered
      keyvals.emplace(key, createThriftValueWithoutBinaryValue(val));
    } else {
      keyvals.emplace(key, val);
    }
  }

  return keyvals;
}

std::vector<std::string>
KvStorePublisher::getFilteredExpiredKeys(
    const std::vector<std::string>& origExpiredKeys) {
  std::vector<std::string> expiredKeys;
  for (auto& key : origExpiredKeys) {
    if (!keyPrefixFilter_.keyMatch(key)) {
      continue;
    }

    expiredKeys.emplace_back(key);
  }
  return expiredKeys;
}

} // namespace openr
