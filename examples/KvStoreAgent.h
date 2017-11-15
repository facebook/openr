/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/zmq/Zmq.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {

class KvStoreAgent : public fbzmq::ZmqEventLoop {
 public:
  KvStoreAgent(
    fbzmq::Context& zmqContext,
    std::string nodeId,
    std::string kvStoreCmdUrl = "tcp://[::1]:60002",
    std::string kvStorePubUrl = "tcp://[::1]:60001");

  ~KvStoreAgent();

  const std::string agentKeyPrefix{"prefixForDataThisAgentDisseminates:"};

 private:
  void
  processKeyVals(const std::map<std::string, thrift::Value>& keyVals);

  std::unique_ptr<KvStoreClient> kvStoreClient_;
  std::unique_ptr<fbzmq::ZmqTimeout> periodicValueChanger_;

}; // class KvStoreAgent
} // namespace openr
