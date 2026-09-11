/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>

#include <fmt/core.h>

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <openr/decision/RouteUpdate.h>
#include <openr/decision/tests/RouteUpdateTestUtils.h>
#include <openr/messaging/ReplicateQueue.h>

/*
 * Sizes the push-time cost of the DecisionRouteUpdate coalescers against a
 * SATURATED backlog: a reader is attached and never drains, so every push after
 * the first runs the coalescer under the reader queue's lock. That is the
 * regime the coalescers exist for -- a stalled consumer -- and it is also their
 * worst case for CPU, since a draining reader would leave the queue empty and
 * skip the merge entirely.
 *
 * The workload shapes mirror the real PrefixManager->Decision producer mix:
 *  - HotKeys:     the steady-state syncKvStore path re-advertising the same
 *                 small prefix set. Untyped, so it coalesces; the big win.
 *  - UniqueKeys:  every push touches a new prefix. Still collapses to one
 *                 element, but that element accumulates every key, so the
 *                 saving is per-element overhead, not per-key memory.
 *  - UniqueDeletes: withdrawals, which reconcile against the pending element's
 *                 delete set. Guards the O(N^2)-under-the-lock regression that
 *                 D117840303 fixed by making the delete fields F14FastSet.
 *  - AlternatingTypes: updates whose prefixType flips every push. prefixType is
 *                 accounting metadata that does not change how Decision applies
 *                 the routes, so the merge just keeps the newest label and this
 *                 collapses like every other shape. Kept as its own shape to
 *                 show that label churn costs nothing.
 *
 * Backlog depth is reported per shape below the timings.
 */

namespace openr {

namespace {

constexpr size_t kNumHotKeys{100};

enum class Shape {
  HotKeys,
  UniqueKeys,
  UniqueDeletes,
  AlternatingTypes,
};

DecisionRouteUpdate
makeUpdate(Shape shape, size_t i) {
  DecisionRouteUpdate update;
  switch (shape) {
  case Shape::HotKeys:
    update.addRouteToUpdate(makeUnicast(makeTestPrefix(i % kNumHotKeys), 1));
    break;
  case Shape::UniqueKeys:
    update.addRouteToUpdate(makeUnicast(makeTestPrefix(i), 1));
    break;
  case Shape::UniqueDeletes:
    update.unicastRoutesToDelete.emplace(makeTestPrefix(i));
    break;
  case Shape::AlternatingTypes:
    update.prefixType =
        (i % 2 == 0) ? thrift::PrefixType::CONFIG : thrift::PrefixType::VIP;
    update.addRouteToUpdate(makeUnicast(makeTestPrefix(i % kNumHotKeys), 1));
    break;
  }
  return update;
}

/*
 * Push `numPushes` updates into a queue whose single reader never drains.
 * Returns the reader's resulting backlog depth.
 */
size_t
pushAll(Shape shape, size_t numPushes, bool coalesce) {
  messaging::ReplicateQueue<DecisionRouteUpdate> q;
  auto reader = q.getReader(
      "decision", coalesce ? coalesceDecisionRouteUpdates : nullptr);
  for (size_t i = 0; i < numPushes; ++i) {
    q.push(makeUpdate(shape, i));
  }
  const auto depth = reader.size();
  q.close();
  return depth;
}

void
benchmarkPushes(uint32_t iters, Shape shape, size_t numPushes, bool coalesce) {
  folly::BenchmarkSuspender suspender;
  while (iters--) {
    /*
     * Build the whole batch up front so the measured region covers only
     * push + coalesce, not RibUnicastEntry construction.
     */
    std::vector<DecisionRouteUpdate> updates;
    updates.reserve(numPushes);
    for (size_t i = 0; i < numPushes; ++i) {
      updates.emplace_back(makeUpdate(shape, i));
    }

    messaging::ReplicateQueue<DecisionRouteUpdate> q;
    auto reader = q.getReader(
        "decision", coalesce ? coalesceDecisionRouteUpdates : nullptr);

    suspender.dismiss();
    for (auto& update : updates) {
      q.push(std::move(update));
    }
    suspender.rehire();

    q.close();
  }
}

const char*
shapeName(Shape shape) {
  switch (shape) {
  case Shape::HotKeys:
    return "HotKeys";
  case Shape::UniqueKeys:
    return "UniqueKeys";
  case Shape::UniqueDeletes:
    return "UniqueDeletes";
  case Shape::AlternatingTypes:
    return "AlternatingTypes";
  }
  return "Unknown";
}

/*
 * The CPU numbers are only meaningful next to the backlog depth they buy, so
 * report the depths once up front.
 *
 * Written to stderr: servicelab parses the folly benchmark table off stdout,
 * and this must not interleave with it.
 */
void
printBacklogSummary(size_t numPushes) {
  fmt::print(
      stderr,
      "\nBacklog depth after {} pushes with a stalled reader:\n",
      numPushes);
  fmt::print(
      stderr, "{:<18} {:>12} {:>12}\n", "shape", "baseline", "coalesced");
  for (const auto shape :
       {Shape::HotKeys,
        Shape::UniqueKeys,
        Shape::UniqueDeletes,
        Shape::AlternatingTypes}) {
    fmt::print(
        stderr,
        "{:<18} {:>12} {:>12}\n",
        shapeName(shape),
        pushAll(shape, numPushes, /*coalesce=*/false),
        pushAll(shape, numPushes, /*coalesce=*/true));
  }
  fmt::print(stderr, "\n");
}

} // namespace

BENCHMARK_NAMED_PARAM(
    benchmarkPushes,
    HotKeys_Baseline,
    Shape::HotKeys,
    10000,
    /*coalesce=*/false);
BENCHMARK_RELATIVE_NAMED_PARAM(
    benchmarkPushes,
    HotKeys_Coalesced,
    Shape::HotKeys,
    10000,
    /*coalesce=*/true);

BENCHMARK_NAMED_PARAM(
    benchmarkPushes,
    UniqueKeys_Baseline,
    Shape::UniqueKeys,
    10000,
    /*coalesce=*/false);
BENCHMARK_RELATIVE_NAMED_PARAM(
    benchmarkPushes,
    UniqueKeys_Coalesced,
    Shape::UniqueKeys,
    10000,
    /*coalesce=*/true);

BENCHMARK_NAMED_PARAM(
    benchmarkPushes,
    UniqueDeletes_Baseline,
    Shape::UniqueDeletes,
    10000,
    /*coalesce=*/false);
BENCHMARK_RELATIVE_NAMED_PARAM(
    benchmarkPushes,
    UniqueDeletes_Coalesced,
    Shape::UniqueDeletes,
    10000,
    /*coalesce=*/true);

BENCHMARK_NAMED_PARAM(
    benchmarkPushes,
    AlternatingTypes_Baseline,
    Shape::AlternatingTypes,
    10000,
    /*coalesce=*/false);
BENCHMARK_RELATIVE_NAMED_PARAM(
    benchmarkPushes,
    AlternatingTypes_Coalesced,
    Shape::AlternatingTypes,
    10000,
    /*coalesce=*/true);

} // namespace openr

int
main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  openr::printBacklogSummary(10000);
  folly::runBenchmarks();
  return 0;
}
