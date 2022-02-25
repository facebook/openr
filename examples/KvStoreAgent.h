/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/OpenrEventBase.h>
#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/kvstore/KvStoreClientInternal.h>

namespace openr {

class KvStoreAgent : public OpenrEventBase {
 public:
  KvStoreAgent(
      std::string nodeId, KvStore<thrift::OpenrCtrlCppAsyncClient>* kvStore);

  const std::string agentKeyPrefix{"prefixForDataThisAgentDisseminates:"};

 private:
  KvStore<thrift::OpenrCtrlCppAsyncClient>* kvStore_{nullptr};
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;
  std::unique_ptr<folly::AsyncTimeout> periodicValueChanger_;

}; // class KvStoreAgent
} // namespace openr
