/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace openr::e2e {

class OpenrE2eLab final {
 public:
  explicit OpenrE2eLab(std::string nodeName);
  ~OpenrE2eLab();

  OpenrE2eLab(const OpenrE2eLab&) = delete;
  OpenrE2eLab& operator=(const OpenrE2eLab&) = delete;

  void addPeer(const std::string& name);
  void start();
  void stop();

  void advertisePrefix(const std::string& peer, const std::string& cidr);
  void withdrawPrefix(const std::string& peer, const std::string& cidr);
  void advertiseLocalPrefix(const std::string& cidr);
  void withdrawLocalPrefix(const std::string& cidr);
  void setLinkUp(const std::string& peer, bool isUp);

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

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openr::e2e
