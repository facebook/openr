/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/PrefixMaterializer.h>

#include <set>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/LsdbUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>

namespace openr::e2e {

std::vector<SerializedKvStoreKey>
materializePrefixes(
    const Topology& topology,
    const folly::F14FastSet<std::string>& selectedNodes) {
  apache::thrift::CompactSerializer serializer;
  std::vector<SerializedKvStoreKey> keys;
  for (const auto& name : topology.nodeNames()) {
    const auto& node = topology.node(name);
    if (node.kind == NodeKind::REAL || !node.operational ||
        (!selectedNodes.empty() && !selectedNodes.contains(name))) {
      continue;
    }
    for (const auto& prefix : node.prefixes) {
      auto entry = createPrefixEntry(toIpPrefix(prefix.prefix), prefix.type);
      entry.tags() =
          std::set<std::string>{prefix.tags.begin(), prefix.tags.end()};
      auto [key, database] = createPrefixKeyAndDb(name, entry, prefix.area);
      auto [_, withdrawal] =
          createPrefixKeyAndDb(name, entry, prefix.area, /*withdraw=*/true);
      keys.push_back(
          {{prefix.area, key.getPrefixKeyV2()},
           name,
           writeThriftObjStr(database, serializer),
           writeThriftObjStr(withdrawal, serializer)});
    }
  }
  return keys;
}

} // namespace openr::e2e
