/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/kvstore/KvStoreClientInternal.h>

namespace openr {

class KvStoreAgent : public OpenrEventBase {
 public:
  KvStoreAgent(
      fbzmq::Context& zmqContext,
      std::string nodeId,
      KvStore* kvStore,
      std::string kvStoreCmdUrl = "tcp://[::1]:60002",
      std::string kvStorePubUrl = "tcp://[::1]:60001");

  const std::string agentKeyPrefix{"prefixForDataThisAgentDisseminates:"};

 private:
  KvStore* kvStore_{nullptr};
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_;
  std::unique_ptr<fbzmq::ZmqTimeout> periodicValueChanger_;

}; // class KvStoreAgent
} // namespace openr
