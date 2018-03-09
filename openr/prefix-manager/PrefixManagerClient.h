/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fbzmq/zmq/Zmq.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include "PrefixManager.h"

namespace openr {

class PrefixManagerClient final {
 public:
  PrefixManagerClient(
      const PrefixManagerLocalCmdUrl& localCmdUrl, fbzmq::Context& context);

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error> addPrefixes(
      const std::vector<thrift::PrefixEntry>& prefixes);

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error> withdrawPrefixes(
      const std::vector<thrift::IpPrefix>& prefixes);

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
  withdrawPrefixesByType(thrift::PrefixType type);

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
  syncPrefixesByType(
      thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error> getPrefixes();

  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
  getPrefixesByType(thrift::PrefixType type);

 private:
  folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error> sendRequest(
      const thrift::PrefixManagerRequest& request);

  // Dealer socket to talk with prefix manager
  fbzmq::Socket<ZMQ_DEALER, fbzmq::ZMQ_CLIENT> prefixManagerCmdSock_;
  apache::thrift::CompactSerializer serializer_;
}; // PrefixManager

} // namespace openr
