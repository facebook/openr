/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/benchmark/OpenrBenchmarkBase.h>

namespace openr {
OpenrBenchmarkBase::OpenrBenchmarkBase() {}

OpenrBenchmarkBase::~OpenrBenchmarkBase() {}

thrift::Value
OpenrBenchmarkBase::createAdjValue(
    const std::string& node,
    int64_t version,
    const std::vector<thrift::Adjacency>& adjs,
    bool overloaded,
    int32_t nodeId,
    int64_t ttlVersion,
    int64_t hashValue) noexcept {
  auto adjDB = createAdjDb(node, adjs, nodeId);
  adjDB.isOverloaded_ref() = overloaded;

  thrift::Value val;
  val.value_ref() = writeThriftObjStr(adjDB, serializer_);
  val.version_ref() = version;
  val.originatorId_ref() = "originator-1";
  val.ttl_ref() = Constants::kTtlInfinity;
  val.ttlVersion_ref() = ttlVersion;
  val.hash_ref() = hashValue;

  return val;
}
} // namespace openr
