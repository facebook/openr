/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/io/async/EventBase.h>

#include <openr/if/gen-cpp2/OpenrCtrlCppAsyncClient.h>
#include <openr/tests/e2e/OpenrE2e.h>
#include <openr/tests/e2e/OpenrKvStoreService.h>
#include <openr/tests/e2e/OpenrQueueBundle.h>
#include <openr/tests/e2e/SyntheticPeer.h>
#include <openr/tests/e2e/TestPlatform.h>

namespace openr {
class MockIoProvider;
}

namespace openr::e2e {

class OpenrModuleGraph;
using E2eCtrlClient = apache::thrift::Client<thrift::OpenrCtrl>;

class OpenrE2eLab::Impl final {
 public:
  explicit Impl(std::string nodeName);
  ~Impl();

  void addPeer(const std::string& name);
  void start();
  void stop();
  void advertisePrefix(const std::string& peer, const std::string& cidr);
  void withdrawPrefix(const std::string& peer, const std::string& cidr);
  void advertiseLocalPrefix(const std::string& cidr);
  void withdrawLocalPrefix(const std::string& cidr);
  void setLinkUp(const std::string& peer, bool isUp);
  void restartFibService();
  void setFibServiceAvailable(bool isAvailable);
  void setLinkMetric(const std::string& peer, std::int32_t metric);
  void clearLinkMetric(const std::string& peer);
  bool awaitAdjacency(
      const std::string& peer, bool isUp, std::chrono::milliseconds timeout);
  bool awaitPeerPrefix(
      const std::string& peer,
      const std::string& cidr,
      bool isAdvertised,
      std::chrono::milliseconds timeout);
  bool awaitProgrammedRoute(
      const std::string& cidr,
      const std::vector<std::string>& viaPeers,
      std::chrono::milliseconds timeout);
  bool awaitRouteAbsent(
      const std::string& cidr, std::chrono::milliseconds timeout);
  bool awaitComputedRoute(
      const std::string& cidr, std::chrono::milliseconds timeout);

 private:
  struct Peer {
    std::string name;
    std::string dutIfName;
    std::string peerIfName;
    int dutIfIndex{0};
    int peerIfIndex{0};
    std::string dutV6;
    std::string peerV6;
    std::unique_ptr<SyntheticPeer> synthetic;
  };

  const Peer& peer(const std::string& name) const;
  void requireStarted() const;
  bool hasProgrammedPrefix(const folly::CIDRNetwork& prefix) const;

  std::string nodeName_;
  OpenrQueueBundle queues_;
  TestPlatform platform_;
  OpenrKvStoreService kvStoreService_;
  std::shared_ptr<MockIoProvider> ioProvider_;
  std::unique_ptr<OpenrModuleGraph> graph_;
  folly::EventBase controlEventBase_;
  std::unique_ptr<E2eCtrlClient> controlClient_;
  std::map<std::string, Peer> peers_;
  bool started_{false};
  bool stopped_{false};
};

} // namespace openr::e2e
