/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <glog/logging.h>

#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/public_tld/examples/KvStoreAgent.h>

namespace openr {

KvStoreAgent::KvStoreAgent(std::string nodeId, KvStore* kvStore)
    : kvStore_(kvStore) {
  CHECK(kvStore_);

  kvStoreClient_ =
      std::make_unique<KvStoreClientInternal>(this, nodeId, kvStore_);

  // set a call back so we can keep track of other keys with the prefix we
  // care about
  kvStoreClient_->setKvCallback(
      [this, nodeId](
          const std::string& key, const std::optional<thrift::Value>& value) {
        if (0 == key.find(agentKeyPrefix) &&
            *value.value().originatorId_ref() != nodeId &&
            value.value().value_ref()) {
          // Lets check out what some other node's value is
          LOG(INFO) << "Got data from: " << *value.value().originatorId_ref()
                    << " Data: " << value.value().value_ref().value();
        }
      });

  // every once in a while, we need to change our value
  periodicValueChanger_ =
      folly::AsyncTimeout::make(*getEvb(), [this, nodeId]() noexcept {
        static int val = 0;
        this->kvStoreClient_->persistKey(
            AreaId{"my_area_name"},
            agentKeyPrefix + nodeId,
            std::to_string(++val));
      });

  periodicValueChanger_->scheduleTimeout(std::chrono::seconds(60));
}

} // namespace openr
