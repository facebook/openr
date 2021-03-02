/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>

#include <openr/common/Constants.h>
#include <openr/common/OpenrClient.h>
#include <openr/public_tld/examples/KvStorePoller.h>

DEFINE_string(host, "::1", "Host to connect to");
DEFINE_int32(port, openr::Constants::kOpenrCtrlPort, "OpenrCtrl server port");

int
main(int argc, char** argv) {
  // Initialize all params
  folly::init(&argc, &argv);

  // Define and start evb
  folly::EventBase evb;

  // Create Open/R client connected through thrift port
  std::vector<folly::SocketAddress> sockAddrs;
  sockAddrs.emplace_back(
      folly::SocketAddress{FLAGS_host, static_cast<uint16_t>(FLAGS_port)});
  auto poller = std::make_unique<openr::KvStorePoller>(sockAddrs);

  LOG(INFO) << "KvStorePoller initialized. Periodic dump will follow";

  evb.runInEventBaseThread([&]() noexcept {
    auto adjDb =
        poller->getAdjacencyDatabases(openr::Constants::kServiceProcTimeout);
    if (not adjDb.first) {
      LOG(ERROR) << "Failed to get adj: database from KvStore";
    } else {
      if (adjDb.first->empty()) {
        LOG(WARNING) << "No adj key available";
      } else {
        LOG(INFO) << "AdjDb dumped with size: " << adjDb.first->size();
      }
    }

    auto prefixDb =
        poller->getPrefixDatabases(openr::Constants::kServiceProcTimeout);
    if (not prefixDb.first) {
      LOG(ERROR) << "Failed to get prefix: database from KvStore";
    } else {
      if (prefixDb.first->empty()) {
        LOG(WARNING) << "No prefix key available";
      } else {
        LOG(INFO) << "PrefixDb dumped with size: " << prefixDb.first->size();
      }
    }

    evb.terminateLoopSoon();
  });

  evb.loopForever();

  return 0;
}
