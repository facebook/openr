/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/e2e/OpenrE2eInternal.h>

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openr/tests/e2e/TopologyMaterializer.h>
#include <openr/tests/e2e/TopologyMutator.h>

namespace openr::e2e {

void
OpenrE2eLab::Impl::mutateTopology(TopologyMutation change) {
  requireStarted();
  if (!topology_) {
    throw std::logic_error("Named topology changes require a topology lab");
  }
  std::vector<TopologyMutation> changes;
  changes.emplace_back(std::move(change));
  auto result = TopologyMutator::apply(*topology_, changes);
  TopologyMaterializer materializer{topologyVersions_};
  const auto updates =
      materializer.diff(*topology_, result.topology, result.affectedNodes);

  if (const auto* metric = std::get_if<SetLinkMetric>(&changes.front());
      metric && metric->node &&
      topology_->node(*metric->node).kind == NodeKind::REAL) {
    const auto& link = result.topology.link(metric->link);
    const auto& endpoint =
        link.first.nodeName == *metric->node ? link.first : link.second;
    controlClient_->sync_setInterfaceMetric(
        endpoint.interfaceName, metric->metric);
  }

  std::map<std::string, thrift::KeyVals> byArea;
  for (const auto& update : updates) {
    if (update.value) {
      byArea[update.key.area].emplace(update.key.key, *update.value);
    }
  }
  for (auto& [_, peer] : peers_) {
    const auto it = byArea.find(peer.area);
    if (it != byArea.end()) {
      peer.synthetic->publishKeyVals(peer.area, it->second);
    }
  }
  topology_ = std::move(result.topology);
}

void
OpenrE2eLab::setLinkMetric(
    const std::string& link, const std::string& endpoint, std::int32_t metric) {
  impl_->mutateTopology(SetLinkMetric{link, endpoint, metric});
}

} // namespace openr::e2e
