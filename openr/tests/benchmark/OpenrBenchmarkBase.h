/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Util.h>

#include <openr/if/gen-cpp2/Types_types.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace openr {

class OpenrBenchmarkBase {
 public:
  OpenrBenchmarkBase();
  virtual ~OpenrBenchmarkBase();

 protected:
  thrift::Value createAdjValue(
      const std::string& node,
      int64_t version,
      const std::vector<thrift::Adjacency>& adjs,
      bool overloaded = false,
      int32_t nodeId = 0,
      int64_t ttlVersion = 0,
      int64_t hashValue = 0) noexcept;

 private:
  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  apache::thrift::CompactSerializer serializer_;
};

} // namespace openr
