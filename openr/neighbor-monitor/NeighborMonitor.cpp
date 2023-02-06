/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/neighbor-monitor/NeighborMonitor.h>

namespace openr {

class NeighborMonitorImpl {};

NeighborMonitor::NeighborMonitor(messaging::ReplicateQueue<AddressEvent>&) {}

void
NeighborMonitor::stop() {
  return;
}

void
NeighborMonitor::run() {
  return;
}

} // namespace openr
