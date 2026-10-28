/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <folly/IPAddress.h>
#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {
class MockIoProvider;
}

namespace openr::e2e {

struct SyntheticPeerConfig {
  std::string nodeName{"node"};
  std::string peerName;
  std::string dutIfName;
  std::string peerIfName;
  int dutIfIndex{0};
  int peerIfIndex{0};
  std::string dutV6Address;
  std::string peerV6Address;
  uint16_t dutKvStorePort{0};
  std::shared_ptr<MockIoProvider> ioProvider;
  std::string area{"0"};
  std::optional<thrift::KeyVals> initialKeyVals;
  std::string transportIfName;
};

class SyntheticPeer final {
 public:
  explicit SyntheticPeer(SyntheticPeerConfig config);
  ~SyntheticPeer();

  SyntheticPeer(const SyntheticPeer&) = delete;
  SyntheticPeer& operator=(const SyntheticPeer&) = delete;

  void start();
  void stopSpark();
  void stop();
  void setOperational(bool operational);
  void advertisePrefix(const std::string& cidr);
  void withdrawPrefix(const std::string& cidr);
  void publishKeyVals(const std::string& area, thrift::KeyVals keyVals);
  bool receivedPrefix(
      const std::string& nodeName,
      const folly::CIDRNetwork& prefix,
      bool isAdvertised) const;

  const std::string& dutIfName() const;
  const std::string& peerV6Address() const;
  uint16_t kvStorePort() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace openr::e2e
