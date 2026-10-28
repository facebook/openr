/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <gtest/gtest.h>
#include <openr/common/LsdbTypes.h>
#include <openr/common/Util.h>
#include <openr/tests/e2e/PrefixMaterializer.h>
#include <openr/tests/e2e/TopologyBuilder.h>

namespace openr::e2e {

TEST(PrefixMaterializerTest, Materialize_IsolatesAreasAndSkipsRealNode) {
  NodeOptions areas;
  areas.areas = {"pod", "plane"};
  PrefixOptions pod;
  pod.area = "pod";
  pod.tags = {"blue"};
  PrefixOptions plane;
  plane.area = "plane";
  const auto topology = TopologyBuilder{"prefixes"}
                            .realNode("r1", areas)
                            .prefix("r1", "10.1.0.0/16", pod)
                            .syntheticNode("r2", areas)
                            .prefix("r2", "10.2.0.0/16", pod)
                            .prefix("r2", "2001:db8::/64", plane)
                            .build();

  const auto keys = materializePrefixes(topology);

  const std::set<std::pair<std::string, std::string>> expected{
      {"pod",
       PrefixKey("r2", folly::IPAddress::createNetwork("10.2.0.0/16"), "pod")
           .getPrefixKeyV2()},
      {"plane",
       PrefixKey(
           "r2", folly::IPAddress::createNetwork("2001:db8::/64"), "plane")
           .getPrefixKeyV2()},
  };
  std::set<std::pair<std::string, std::string>> actual;
  for (const auto& key : keys) {
    actual.emplace(key.key.area, key.key.key);
  }
  EXPECT_EQ(expected, actual);
  ASSERT_EQ(expected.size(), keys.size());

  const auto podKey =
      std::find_if(keys.begin(), keys.end(), [](const auto& key) {
        return key.key.area == "pod";
      });
  ASSERT_NE(keys.end(), podKey);
  apache::thrift::CompactSerializer serializer;
  const auto database =
      readThriftObjStr<thrift::PrefixDatabase>(podKey->data, serializer);
  EXPECT_EQ("r2", *database.thisNodeName());
  EXPECT_EQ(1, database.prefixEntries()->size());
  EXPECT_EQ(1, database.prefixEntries()->front().tags()->size());
}

} // namespace openr::e2e
