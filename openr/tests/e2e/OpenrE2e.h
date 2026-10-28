/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <openr/tests/e2e/TopologyTypes.h>

namespace openr::e2e {

class Topology;

class OpenrE2eLab final {
 public:
  explicit OpenrE2eLab(std::string nodeName);
  explicit OpenrE2eLab(const Topology& topology);
  ~OpenrE2eLab();

  OpenrE2eLab(const OpenrE2eLab&) = delete;
  OpenrE2eLab& operator=(const OpenrE2eLab&) = delete;

  void addPeer(const std::string& name);
  void start();
  void stop();

  void advertisePrefix(const std::string& peer, const std::string& cidr);
  void advertisePrefix(
      const std::string& node, const std::string& cidr, PrefixOptions options);
  void withdrawPrefix(const std::string& peer, const std::string& cidr);
  void withdrawPrefix(
      const std::string& node,
      const std::string& cidr,
      const PrefixOptions& options);
  void advertiseLocalPrefix(const std::string& cidr);
  void withdrawLocalPrefix(const std::string& cidr);
  void setLinkUp(const std::string& peer, bool isUp);
  void setLinkState(const std::string& link, bool isUp);
  void setPartition(const std::vector<std::string>& links, bool isPartitioned);
  void restartFibService();
  void setFibServiceAvailable(bool isAvailable);
  void setLinkMetric(const std::string& peer, std::int32_t metric);
  void setLinkMetric(
      const std::string& link,
      const std::string& endpoint,
      std::int32_t metric);
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
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openr::e2e
