/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/OpenrEventBase.h>
#include <openr/kvstore/KvStoreClientInternal.h>

namespace openr {

class KvStoreAgent : public OpenrEventBase {
 public:
  KvStoreAgent(std::string nodeId, KvStore* kvStore);

  const std::string agentKeyPrefix{"prefixForDataThisAgentDisseminates:"};

 private:
  KvStore* kvStore_{nullptr};
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;
  std::unique_ptr<folly::AsyncTimeout> periodicValueChanger_;

}; // class KvStoreAgent
} // namespace openr
