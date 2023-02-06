/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/LsdbTypes.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/messaging/ReplicateQueue.h>

namespace openr {

class NeighborMonitorImpl;

class NeighborMonitor : public OpenrEventBase {
 public:
  explicit NeighborMonitor(
      messaging::ReplicateQueue<AddressEvent>& addrEventQueue);

  NeighborMonitor(const NeighborMonitor&) = delete;

  NeighborMonitor& operator=(const NeighborMonitor&) = delete;

  ~NeighborMonitor() = default;

  void stop() override;

  void run() override;

 private:
  std::shared_ptr<NeighborMonitorImpl> impl_;

#ifdef NeighborMonitor_TEST_FRIENDS
  NeighborMonitor_TEST_FRIENDS
#endif
};

} // namespace openr
