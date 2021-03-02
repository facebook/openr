/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <chrono>

#include <folly/dynamic.h>
#include <folly/json.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <openr/monitor/LogSample.h>

namespace openr {

TEST(LogSampleTest, ApiTest) {
  const auto timestamp =
      std::chrono::system_clock::time_point(std::chrono::seconds(111));
  LogSample sample(timestamp);

  const std::vector<std::string> values = {{"val1", "val2", "val3"}};
  const std::set<std::string> tags = {{"tag1", "tag2", "tag3"}};

  sample.addInt("int-key", 123);
  sample.addDouble("double-key", 123.456);
  sample.addString("string-key", "hello world");
  sample.addStringVector("vector-key", values);
  sample.addStringTagset("tagset-key", tags);

  EXPECT_TRUE(sample.isIntSet("time")); // NOTE: Special time key
  EXPECT_TRUE(sample.isIntSet("int-key"));
  EXPECT_TRUE(sample.isDoubleSet("double-key"));
  EXPECT_TRUE(sample.isStringSet("string-key"));
  EXPECT_TRUE(sample.isStringVectorSet("vector-key"));
  EXPECT_TRUE(sample.isStringTagsetSet("tagset-key"));

  EXPECT_EQ(111, sample.getInt("time")); // NOTE: Special time key
  EXPECT_EQ(123, sample.getInt("int-key"));
  EXPECT_EQ(123.456, sample.getDouble("double-key"));
  EXPECT_EQ("hello world", sample.getString("string-key"));
  EXPECT_EQ(values, sample.getStringVector("vector-key"));
  EXPECT_EQ(tags, sample.getStringTagset("tagset-key"));

  // Verify some false behaviours
  EXPECT_FALSE(sample.isIntSet("double-key"));
  EXPECT_FALSE(sample.isDoubleSet("vector-key"));
  EXPECT_FALSE(sample.isStringTagsetSet("vector-key"));
  EXPECT_THROW(sample.getDouble("int-key"), std::invalid_argument);
  EXPECT_THROW(sample.getInt("vector-key"), std::invalid_argument);

  // Verify json output
  LOG(INFO) << sample.toJson();
  const std::string jsonSample = R"config(
    {
     "int":{
        "time":111,
        "int-key":123
     },
     "normvector":{
        "vector-key":[
           "val1",
           "val2",
           "val3"
        ]
     },
     "normal":{
        "string-key":"hello world"
     },
     "double":{
        "double-key":123.456
     },
     "tagset":{
        "tagset-key":[
           "tag1",
           "tag2",
           "tag3"
        ]
     }
    }
  )config";

  folly::json::serialization_opts opts;
  opts.sort_keys = true;
  auto expectedJson = folly::parseJson(jsonSample);
  EXPECT_EQ(folly::json::serialize(expectedJson, opts), sample.toJson());
}

TEST(LogSampleTest, fromJsonTest) {
  const std::string jsonSample = R"config(
    {
     "int":{
        "time":111,
        "int-key":123
     },
     "normvector":{
        "vector-key":[
           "val1",
           "val2",
           "val3"
        ]
     },
     "normal":{
        "string-key":"hello world"
     },
     "double":{
        "double-key":123.456
     },
     "tagset":{
        "tagset-key":[
           "tag1",
           "tag2",
           "tag3"
        ]
     }
    }
  )config";
  auto sample = LogSample::fromJson(jsonSample);
  EXPECT_TRUE(sample.isIntSet("time")); // NOTE: Special time key
  EXPECT_TRUE(sample.isIntSet("int-key"));
  EXPECT_TRUE(sample.isDoubleSet("double-key"));
  EXPECT_TRUE(sample.isStringSet("string-key"));
  EXPECT_TRUE(sample.isStringVectorSet("vector-key"));
  EXPECT_TRUE(sample.isStringTagsetSet("tagset-key"));

  EXPECT_EQ(111, sample.getInt("time")); // NOTE: Special time key
  EXPECT_EQ(123, sample.getInt("int-key"));
  EXPECT_EQ(123.456, sample.getDouble("double-key"));
  EXPECT_EQ("hello world", sample.getString("string-key"));
  const std::vector<std::string> values = {{"val1", "val2", "val3"}};
  const std::set<std::string> tags = {{"tag1", "tag2", "tag3"}};
  EXPECT_EQ(values, sample.getStringVector("vector-key"));
  EXPECT_EQ(tags, sample.getStringTagset("tagset-key"));

  // Verify some false behaviours
  EXPECT_FALSE(sample.isIntSet("double-key"));
  EXPECT_FALSE(sample.isDoubleSet("vector-key"));
  EXPECT_FALSE(sample.isStringTagsetSet("vector-key"));
  EXPECT_THROW(sample.getDouble("int-key"), std::invalid_argument);
  EXPECT_THROW(sample.getInt("vector-key"), std::invalid_argument);

  const std::string jsonSampleNoTimeKey = R"config(
    {
     "int":{
        "not-time":111,
        "int-key":123
     }
    }
  )config";
  EXPECT_THROW(LogSample::fromJson(jsonSampleNoTimeKey), std::exception);
}

} // namespace openr

int
main(int argc, char** argv) {
  // Basic initialization
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  // Run the tests
  return RUN_ALL_TESTS();
}
