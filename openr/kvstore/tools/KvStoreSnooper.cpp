/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>

#include <folly/init/Init.h>

#include <openr/common/OpenrClient.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreUtil.h>

DEFINE_string(host, "::1", "Host to connect to");
DEFINE_int32(port, openr::Constants::kOpenrCtrlPort, "OpenrCtrl server port");
DEFINE_int32(connect_timeout_ms, 1000, "Connect timeout for client");
DEFINE_int32(processing_timeout_ms, 5000, "Processing timeout for client");

int
main(int argc, char** argv) {
  // Initialize all params
  folly::init(&argc, &argv);

  // Define and start event base
  folly::EventBase evb;
  std::thread evbThread([&evb]() { evb.loopForever(); });

  // Create Open/R client
  auto client =
      openr::getOpenrCtrlPlainTextClient<apache::thrift::RocketClientChannel>(
          evb,
          folly::IPAddress(FLAGS_host),
          FLAGS_port,
          std::chrono::milliseconds(FLAGS_connect_timeout_ms),
          std::chrono::milliseconds(FLAGS_processing_timeout_ms));
  auto response = client->semifuture_subscribeAndGetAreaKvStores({}, {}).get();
  std::unordered_map<
      std::string /* area */,
      std::unordered_map<std::string /* key */, openr::thrift::Value>>
      areaKeyVals;
  LOG(INFO) << "Stream is connected, updates will follow";
  for (auto const& pub : response.response) {
    LOG(INFO) << "Received " << pub.get_keyVals().size()
              << " entries in initial dump for area: " << pub.get_area();
    areaKeyVals[pub.get_area()] = pub.get_keyVals();
  }
  LOG(INFO) << "";

  auto subscription =
      std::move(response.stream)
          .subscribeExTry(
              folly::Executor::getKeepAliveToken(&evb),
              [areaKeyVals = std::move(areaKeyVals)](
                  folly::Try<openr::thrift::Publication>&& maybePub) mutable {
                if (maybePub.hasException()) {
                  LOG(ERROR) << maybePub.exception().what();
                  return;
                }
                auto& pub = maybePub.value();
                // Print expired key-vals
                for (const auto& key : *pub.expiredKeys_ref()) {
                  std::cout << "Expired Key: " << key << std::endl;
                  std::cout << "" << std::endl;
                }

                // Print updates
                auto updatedKeyVals = openr::mergeKeyValues(
                    areaKeyVals.at(pub.get_area()), *pub.keyVals_ref());
                for (auto& [key, val] : updatedKeyVals) {
                  std::cout
                      << (val.value_ref().has_value() ? "Updated" : "Refreshed")
                      << " KeyVal: " << key << std::endl;
                  std::cout << "  version: " << *val.version_ref() << std::endl;
                  std::cout << "  originatorId: " << *val.originatorId_ref()
                            << std::endl;
                  std::cout << "  ttl: " << *val.ttl_ref() << std::endl;
                  std::cout << "  ttlVersion: " << *val.ttlVersion_ref()
                            << std::endl;
                  std::cout << "  hash: " << val.hash_ref().value() << std::endl
                            << std::endl; // intended
                }
              });

  evbThread.join();
  subscription.cancel();
  std::move(subscription).detach();
  client.reset();

  return 0;
}
