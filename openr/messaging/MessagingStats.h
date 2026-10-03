/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <fb303/QuantileStat.h>

namespace openr::messaging {

/*
 * Rolling-window per-reader queue-dwell samples (integer nanoseconds, the
 * native steady_clock capture precision). Exports
 * `messaging.rw_queue.<queue>-<reader>.time_spent_ns.{count,avg,p50,p95,p99,p100}.60`
 * via fb303/ODS, following the BGP quantile-stat pattern (`count.60` lets
 * dashboards distinguish empty windows from zero dwell; `p100.60` is the
 * rolling-window max that replaces the deleted lifetime `time_spent_max_ms`
 * without its never-decays problem). Nanosecond units keep full capture
 * precision with no per-sample scaling.
 *
 * Resolve (registering on first use) the dwell quantile stat for one
 * queue/reader. Capture the result in the per-reader sink (see
 * bindRwQueueDwellSink) so pops pay a single thread-safe addValue with no
 * key hashing or stat lookup under the queue lock.
 */
std::shared_ptr<facebook::fb303::QuantileStat> getRwQueueDwellStat(
    const std::string& queueName, const std::string& readerId);

/*
 * Bind one reader queue's dwell-sample sink to its rolling-window quantile
 * stat under the given queue name. An empty name (queue not yet registered
 * for monitoring) clears the sink rather than exporting under an empty
 * prefix. The stat is resolved once here so pops skip the per-sample
 * lookup. Callers must hold the reader-list lock; re-invoke for all
 * readers when the name is (re)set.
 */
template <typename QueueT>
void
bindRwQueueDwellSink(
    const std::shared_ptr<QueueT>& reader, const std::string& queueName) {
  if (queueName.empty()) {
    reader->setDwellSampleSink(nullptr);
    return;
  }
  auto stat = getRwQueueDwellStat(queueName, reader->getQueueId());
  reader->setDwellSampleSink(
      [stat](double dwellNs) { stat->addValue(dwellNs); });
}

} // namespace openr::messaging
