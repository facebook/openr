/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/tests/scale/LeakProbe.h>

#include <string_view>

#include <fmt/format.h>

namespace openr {

std::vector<ChurnRound>
buildTransientAreaChurn(
    const std::string& areaPrefix, int rounds, const thrift::KeyVals& keyVals) {
  std::vector<ChurnRound> plan;
  if (rounds <= 0) {
    return plan;
  }
  plan.reserve(rounds);
  for (int i = 0; i < rounds; ++i) {
    plan.push_back(ChurnRound{fmt::format("{}-{}", areaPrefix, i), keyVals});
  }
  return plan;
}

const std::vector<std::string>&
leakSignatureCounters() {
  static const std::vector<std::string> kCounters{
      "process.memory.rss",
      "decision.num_complete_adjacencies",
  };
  return kCounters;
}

std::string
leakSignatureRegex() {
  /*
   * std::regex (ECMAScript) metacharacters that must be backslash-escaped so a
   * counter name matches literally inside the alternation. Today's names only
   * contain '.', but escaping the full set keeps the regex correct if a name
   * with other metacharacters is ever added to leakSignatureCounters().
   */
  static constexpr std::string_view kMetachars{R"RE(.^$|()[]{}*+?\)RE"};
  std::string regex;
  for (const auto& name : leakSignatureCounters()) {
    if (!regex.empty()) {
      regex += "|";
    }
    for (const char c : name) {
      if (kMetachars.find(c) != std::string_view::npos) {
        regex += '\\';
      }
      regex += c;
    }
  }
  return regex;
}

GrowthReport
analyzeCounterGrowth(
    const std::vector<CounterSample>& samples,
    const std::vector<std::string>& watch) {
  GrowthReport report;
  for (const auto& name : watch) {
    CounterGrowth g;
    bool any = false;
    int64_t prev = 0;
    for (const auto& sample : samples) {
      auto it = sample.counters.find(name);
      if (it == sample.counters.end()) {
        continue;
      }
      const int64_t v = it->second;
      if (!any) {
        g.first = v;
        any = true;
      } else if (v < prev) {
        g.monotonicNonDecreasing = false;
      }
      g.last = v;
      prev = v;
    }
    g.present = any;
    g.delta = any ? (g.last - g.first) : 0;
    report.perCounter[name] = g;
    if (any && g.monotonicNonDecreasing && g.delta > 0) {
      report.leaking.push_back(name);
    }
  }
  return report;
}

} // namespace openr
