/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <openr/if/gen-cpp2/KvStore_types.h>

namespace openr {

/*
 * Instrumentation for the Decision `areaLinkStates_` leak.
 *
 * The DUT's Decision creates one LinkState per area it ever receives a KvStore
 * publication for (lazily, keyed by the publication's non-empty `area`) and
 * never erases it -- there is no area-removal path. So injecting publications
 * for an ever-growing set of distinct area names makes the DUT accumulate
 * LinkStates without bound.
 *
 * This module provides (1) the churn workload that provokes the leak and (2)
 * the counter-growth analysis that detects it. Both are pure so they are
 * unit-tested in CI; the live loop that ties them to a DUT is:
 *
 *   auto plan = buildTransientAreaChurn(prefix, rounds, baseKeyVals);
 *   std::vector<CounterSample> samples;
 *   for (int i = 0; i < plan.size(); ++i) {
 *     injector.injectKeyVals(plan[i].keyVals, plan[i].area);
 *     samples.push_back({i, monitor.getRegexCounters(leakSignatureRegex())});
 *   }
 *   auto report = analyzeCounterGrowth(samples, leakSignatureCounters());
 *
 * Each injectKeyVals into a fresh area grows the DUT's areaLinkStates_ by one;
 * report.leaking then lists the counters that grew and never recovered.
 */

/*
 * One churn round: a fresh, distinct area name and the key-vals to inject into
 * it. Injecting the round drives the DUT to create one more (never-freed)
 * LinkState.
 */
struct ChurnRound {
  std::string area;
  thrift::KeyVals keyVals;
};

/*
 * Build `rounds` churn rounds, each targeting a fresh distinct area named
 * "<areaPrefix>-<i>" (i in [0, rounds)) and carrying a copy of `keyVals` so the
 * injection is a valid, non-empty publication for that area. Injecting every
 * round therefore grows the DUT's areaLinkStates_ by `rounds`. `rounds` <= 0
 * yields an empty plan.
 */
std::vector<ChurnRound> buildTransientAreaChurn(
    const std::string& areaPrefix, int rounds, const thrift::KeyVals& keyVals);

/*
 * The counters whose unbounded, never-recovering growth across churn rounds is
 * the signature of the areaLinkStates_ leak. "process.memory.rss" grows because
 * each leaked LinkState retains memory; "decision.num_complete_adjacencies"
 * grows because Decision sums it over ALL areas, including stale ones.
 * "decision.num_nodes" is deliberately excluded: it dedups node names across
 * areas, so re-injecting the same nodes into new areas leaves it flat.
 */
const std::vector<std::string>& leakSignatureCounters();

/*
 * A regex (OR of leakSignatureCounters()) suitable for
 * DutMonitor::getRegexCounters() to pull exactly the signature counters.
 */
std::string leakSignatureRegex();

/* One pull of the DUT's counters at a given churn round. */
struct CounterSample {
  int32_t round{0};
  std::map<std::string, int64_t> counters;
};

/* Per-counter trend across all samples. */
struct CounterGrowth {
  int64_t first{0};
  int64_t last{0};
  int64_t delta{0}; // last - first
  /* True iff the value never dropped between consecutive samples. */
  bool monotonicNonDecreasing{true};
  /* True iff the counter appeared in at least one sample. */
  bool present{false};
};

struct GrowthReport {
  std::map<std::string, CounterGrowth> perCounter;
  /*
   * Watched counters that grew net-positive AND never decreased across the run
   * -- the signature of a never-reclaimed resource (the areaLinkStates_ leak).
   */
  std::vector<std::string> leaking;
};

/*
 * Analyze the ordered `samples` for each counter in `watch`. A counter is
 * flagged as leaking when it is present, never decreases between consecutive
 * samples, and ends strictly above where it started. Counters that oscillate
 * (ever drop) or are flat are not flagged.
 */
GrowthReport analyzeCounterGrowth(
    const std::vector<CounterSample>& samples,
    const std::vector<std::string>& watch);

} // namespace openr
