/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <gtest/gtest.h>

#include <openr/common/Util.h>
#include <openr/kvstore/KvStorePublisher.h>

using namespace openr;

TEST(KvStorePublisher, OnlyExpiredKeysFilterTest) {
  // dummy settings
  std::set<std::string> selectAreas;

  thrift::KeyDumpParams filter;
  std::vector<std::string> keys = {Constants::kAdjDbMarker.toString()};
  // We only keep the keys with prefix "adj:"
  filter.keys() = keys;

  auto streamAndPublisher =
      apache::thrift::ServerStream<thrift::Publication>::createPublisher();

  auto kvStorePublisher = std::make_unique<KvStorePublisher>(
      selectAreas,
      std::move(filter),
      std::move(streamAndPublisher.second),
      std::chrono::steady_clock::now(),
      0);

  thrift::Publication publication;
  publication.expiredKeys()->emplace_back("adj:test1");
  publication.expiredKeys()->emplace_back("prefix:test2");

  publication.area() = "default";

  kvStorePublisher->publish(publication);

  kvStorePublisher->complete();

  // check the received keys
  const RegexSet prefixMatcher(keys);
  std::move(streamAndPublisher.first)
      .toClientStreamUnsafeDoNotUse()
      .subscribeInline(
          [&prefixMatcher](
              folly::Try<thrift::Publication>&& receivedPublication) {
            if (receivedPublication.hasValue()) {
              // we wil receive one key: adj:test1
              EXPECT_EQ(1, receivedPublication->expiredKeys()->size());
              for (auto& key : *receivedPublication->expiredKeys()) {
                EXPECT_TRUE(prefixMatcher.match(key));
              }
            }
          });
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  const folly::Init init(&argc, &argv);
  FLAGS_logtostderr = true;

  // Run the tests
  return RUN_ALL_TESTS();
}
