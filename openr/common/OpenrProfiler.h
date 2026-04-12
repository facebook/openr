/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/CPortability.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/stats/Histogram.h>

#include <re2/re2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

DECLARE_bool(openr_profiler_enabled);

namespace openr {

/*
 * Thread identifier for lock-free profiling.
 * OpenR has 3 main module threads: KvStore, Decision, and Fib.
 * Values are array indices — do not reorder without updating kNumThreadTypes.
 */
enum class ProfilerThread : size_t {
  KVSTORE = 0,
  DECISION = 1,
  FIB = 2,
};

inline constexpr size_t kNumThreadTypes = 3;

/*
 * Per-function statistics with per-thread lock-free histograms.
 * Each thread writes to its own ThreadData — zero locking on the write path.
 * Merging only happens on stats read.
 */
struct ProfileStat {
  std::atomic<uint64_t> count{0};

  struct ThreadData {
    folly::Histogram<int64_t> histogram;
    int64_t maxUs{0};
    int64_t totalUs{0};
    ThreadData();
  };

  std::array<ThreadData, kNumThreadTypes> threadData;

  ProfileStat();

  ThreadData&
  getThreadData(ProfilerThread thread) {
    return threadData[static_cast<size_t>(thread)];
  }

  const ThreadData&
  getThreadData(ProfilerThread thread) const {
    return threadData[static_cast<size_t>(thread)];
  }

  folly::Histogram<int64_t> getMergedHistogram() const;
  int64_t getMergedMaxUs() const;
  int64_t getMergedTotalUs() const;
};

/*
 * Singleton profiler for Open/R processing loops
 */
class OpenrProfiler {
 public:
  static OpenrProfiler* getInstance();

  /*
   * Control - gflag at startup, Thrift at runtime
   */
  void setEnabled(bool enabled);
  bool
  isEnabled() const {
    return enabled_.load(std::memory_order_relaxed);
  }

  /*
   * Optional regex filter to limit which functions are profiled.
   * Uses pre-computed matching for O(1) hot path lookups.
   */
  void setFilterRegex(const std::string& regexStr);
  bool matchesFilter(std::string_view name);
  bool
  hasFilter() const {
    return hasFilter_.load(std::memory_order_relaxed);
  }

  /*
   * Thread registration for lock-free profiling.
   * Call once at thread startup to identify which thread is running.
   */
  void setCurrentThread(ProfilerThread thread);
  ProfilerThread getCurrentThread() const;

  /*
   * Recording - called by ScopedProfile destructor
   */
  void recordFinish(std::string_view name, std::chrono::nanoseconds duration);

  /*
   * Stats retrieval
   */
  struct StatSummary {
    std::string name;
    uint64_t count;
    int64_t p50Ms;
    int64_t p90Ms;
    int64_t p99Ms;
    int64_t maxMs;
    int64_t totalMs;
  };

  std::vector<StatSummary> getStats() const;
  void clearStats();

  /*
   * ODS export - publishes histogram percentiles as fb303 counters
   */
  void exportToOds();

 private:
  OpenrProfiler() = default;

  std::shared_ptr<ProfileStat> getStat(std::string_view name);

  std::atomic<bool> enabled_{false};
  std::atomic<bool> hasFilter_{false};
  folly::Synchronized<std::unique_ptr<re2::RE2>> filterRegex_;
  folly::Synchronized<
      folly::F14FastMap<std::string, std::shared_ptr<ProfileStat>>>
      stats_;

  /*
   * Pre-computed filter results for O(1) hot path lookups.
   * Maps function name → match result (true = matched, false = checked but
   * not matched). Using a single map eliminates the TOCTOU race that existed
   * with separate checkedNames_ / matchedNames_ sets.
   * Cleared when filter changes.
   */
  folly::Synchronized<folly::F14FastMap<std::string, bool>> filterCache_;
};

/*
 * RAII helper for profiling synchronous functions.
 * Prefer the OPENR_PROFILE macro which generates unique variable names.
 *
 * Disabled path cost: one atomic load + one predicted branch (~2 cycles).
 * Enabled path: string_view avoids heap allocation for the function name.
 */
class ScopedProfile {
 public:
  explicit inline ScopedProfile(const char* name) noexcept : name_(name) {
    auto* profiler = OpenrProfiler::getInstance();
    if (FOLLY_LIKELY(!profiler->isEnabled())) {
      return;
    }
    recordStart(profiler);
  }

  inline ~ScopedProfile() noexcept {
    if (FOLLY_UNLIKELY(shouldRecord_)) {
      recordEnd();
    }
  }

  ScopedProfile(const ScopedProfile&) = delete;
  ScopedProfile& operator=(const ScopedProfile&) = delete;
  ScopedProfile(ScopedProfile&&) = delete;
  ScopedProfile& operator=(ScopedProfile&&) = delete;

 private:
  void recordStart(OpenrProfiler* profiler) noexcept;
  void recordEnd() noexcept;

  const char* name_;
  std::chrono::steady_clock::time_point start_;
  bool shouldRecord_{false};
};

/*
 * Macro wrapper for ScopedProfile.
 * Generates unique variable names via __LINE__ so multiple profiles
 * can coexist in a single scope.
 *
 * Usage:
 *   void processUpdate() {
 *     OPENR_PROFILE("KvStore::processUpdate");
 *     // ... function body
 *   }
 */
#define OPENR_PROFILE_CONCAT_IMPL_(a, b) a##b
#define OPENR_PROFILE_CONCAT_(a, b) OPENR_PROFILE_CONCAT_IMPL_(a, b)
#define OPENR_PROFILE(name) \
  ::openr::ScopedProfile OPENR_PROFILE_CONCAT_(_openr_prof_, __LINE__)(name)

} // namespace openr
