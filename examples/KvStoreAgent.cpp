/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>

#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/public_tld/examples/KvStoreAgent.h>

namespace openr {

KvStoreAgent::KvStoreAgent(
    std::string nodeId, KvStore<thrift::OpenrCtrlCppAsyncClient>* kvStore)
    : kvStore_(kvStore) {
  CHECK(kvStore_);

  kvStoreClient_ =
      std::make_unique<KvStoreClientInternal>(this, nodeId, kvStore_);

  // subscribe to prefixes of this specific node that we care about
  const auto keyPrefix =
      fmt::format("{}{}:", Constants::kPrefixDbMarker.toString(), nodeId);
  kvStoreClient_->subscribeKeyFilter(
      KvStoreFilters({keyPrefix}, {} /* originatorIds */),
      [this, nodeId](
          const std::string& key, const std::optional<thrift::Value>& value) {
        if (0 == key.find(agentKeyPrefix) &&
            *value.value().originatorId() != nodeId && value.value().value()) {
          // Lets check out what some other node's value is
          LOG(INFO) << "Got data from: " << *value.value().originatorId()
                    << " Data: " << value.value().value().value();
        }
      });

  // every once in a while, we need to change our value
  periodicValueChanger_ =
      folly::AsyncTimeout::make(*getEvb(), [this, nodeId]() noexcept {
        static int val = 0;
        this->kvStoreClient_->setKey(
            AreaId{"my_area_name"},
            agentKeyPrefix + nodeId,
            std::to_string(++val));
      });

  periodicValueChanger_->scheduleTimeout(std::chrono::seconds(60));
}

} // namespace openr
