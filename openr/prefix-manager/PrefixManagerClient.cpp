/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixManagerClient.h"

#include <folly/Expected.h>
#include <folly/gen/Base.h>

#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

PrefixManagerClient::PrefixManagerClient(
    const PrefixManagerLocalCmdUrl& localCmdUrl, fbzmq::Context& context)
    : prefixManagerCmdSock_{context} {
  CHECK(prefixManagerCmdSock_.connect(fbzmq::SocketUrl{localCmdUrl}));
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::addPrefixes(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::ADD_PREFIXES;
  req.prefixes = prefixes;
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::withdrawPrefixes(
    const std::vector<thrift::IpPrefix>& prefixes) {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::WITHDRAW_PREFIXES;
  req.prefixes =
      folly::gen::from(prefixes) |
      folly::gen::mapped([](const thrift::IpPrefix& prefix) {
        return thrift::PrefixEntry(apache::thrift::FRAGILE, prefix, {}, {});
      }) |
      folly::gen::as<std::vector<thrift::PrefixEntry>>();
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::withdrawPrefixesByType(thrift::PrefixType type) {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::WITHDRAW_PREFIXES_BY_TYPE;
  req.type = type;
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::syncPrefixesByType(
    thrift::PrefixType type, const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::SYNC_PREFIXES_BY_TYPE;
  req.prefixes = prefixes;
  req.type = type;
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::getPrefixes() {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::GET_ALL_PREFIXES;
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::getPrefixesByType(thrift::PrefixType type) {
  thrift::PrefixManagerRequest req;
  req.cmd = thrift::PrefixManagerCommand::GET_PREFIXES_BY_TYPE;
  req.type = type;
  return sendRequest(req);
}

folly::Expected<thrift::PrefixManagerResponse, fbzmq::Error>
PrefixManagerClient::sendRequest(const thrift::PrefixManagerRequest& request) {
  auto maybeReqMsg = fbzmq::Message::fromThriftObj(request, serializer_);
  if (maybeReqMsg.hasError()) {
    return folly::makeUnexpected(maybeReqMsg.error());
  }

  // send empty delimiter followed by request
  auto maybeSize =
      prefixManagerCmdSock_.sendMultiple(fbzmq::Message(), maybeReqMsg.value());
  if (maybeSize.hasError()) {
    return folly::makeUnexpected(maybeSize.error());
  }

  // Receive response
  fbzmq::Message delimMsg, thriftRepMsg;
  const auto ret = prefixManagerCmdSock_.recvMultiple(delimMsg, thriftRepMsg);

  if (ret.hasError()) {
    LOG(ERROR) << "processRequest: Error receiving command: " << ret.error();
    return folly::makeUnexpected(ret.error());
  }

  if (not delimMsg.empty()) {
    LOG(ERROR) << "processRequest: Non-empty delimiter";
    return folly::makeUnexpected(fbzmq::Error());
  }

  return thriftRepMsg.readThriftObj<thrift::PrefixManagerResponse>(serializer_);
}

} // namespace openr
