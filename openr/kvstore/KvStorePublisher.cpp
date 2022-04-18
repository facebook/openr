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
  std::vector<std::string> keyPrefix;

  if (filter.keys_ref().has_value()) {
    keyPrefix = std::move(*filter.keys_ref());
  } else if (filter.prefix_ref().is_set()) {
    folly::split(",", *filter.prefix_ref(), keyPrefix, true);
  }

  thrift::FilterOperator op = filter_.oper_ref().has_value()
      ? *filter_.oper_ref()
      : thrift::FilterOperator::OR;

  keyPrefixFilter_ =
      KvStoreFilters(keyPrefix, std::move(*filter.originatorIds_ref()), op);
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
  if (not(selectAreas_.empty() || selectAreas_.count(*pub.area_ref()))) {
    return;
  }
  if ((not filter_.keys_ref().has_value() or (*filter_.keys_ref()).empty()) and
      (not filter_.originatorIds_ref().is_set() or
       (*filter_.originatorIds_ref()).empty()) and
      not *filter_.ignoreTtl_ref() and not *filter_.doNotPublishValue_ref()) {
    // No filtering criteria. Accept all updates as TTL updates are not be
    // to be updated. If we don't optimize here, we will have go through
    // key values of a publication and copy them.
    auto filteredPub = std::make_unique<thrift::Publication>(pub);
    filteredPub->timestamp_ms_ref() = getUnixTimeStampMs();
    publisher_.next(std::move(*filteredPub));
    return;
  }

  thrift::Publication publication_filtered;
  publication_filtered.area_ref() = *pub.area_ref();
  publication_filtered.keyVals_ref() = getFilteredKeyVals(*pub.keyVals_ref());
  publication_filtered.expiredKeys_ref() =
      getFilteredExpiredKeys(*pub.expiredKeys_ref());

  if (pub.nodeIds_ref()) {
    publication_filtered.nodeIds_ref() = *pub.nodeIds_ref();
  }

  if (pub.tobeUpdatedKeys_ref()) {
    publication_filtered.tobeUpdatedKeys_ref() = *pub.tobeUpdatedKeys_ref();
  }

  if (publication_filtered.keyVals_ref()->size() or
      publication_filtered.expiredKeys_ref()->size()) {
    // There is at least one key value in the publication for the client
    // or there are some expiredKeys
    publication_filtered.timestamp_ms_ref() = getUnixTimeStampMs();
    publisher_.next(std::move(publication_filtered));
  }
}

thrift::KeyVals
KvStorePublisher::getFilteredKeyVals(const thrift::KeyVals& origKeyVals) {
  // The value field may be explicitly excluded/ignored by the doNotPublishValue
  // flag
  thrift::KeyVals keyvals;
  for (auto& [key, val] : origKeyVals) {
    if (*filter_.ignoreTtl_ref() and not val.value_ref().has_value()) {
      // ignore TTL updates
      continue;
    }

    if (not keyPrefixFilter_.keyMatch(key, val)) {
      continue;
    }

    if (*filter_.doNotPublishValue_ref() and val.value_ref().has_value()) {
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
    if (not keyPrefixFilter_.keyMatch(key)) {
      continue;
    }

    expiredKeys.emplace_back(key);
  }
  return expiredKeys;
}

} // namespace openr
