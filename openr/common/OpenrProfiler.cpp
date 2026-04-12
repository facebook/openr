/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/OpenrProfiler.h>

#include <fb303/ServiceData.h>
#include <folly/logging/xlog.h>

DEFINE_bool(
    openr_profiler_enabled,
    false,
    "Enable Open/R profiler (disabled by default, enable via gflag or Thrift)");

DEFINE_bool(openr_profiler_export_ods, false, "Export profiler stats to ODS");

namespace fb303 = facebook::fb303;

namespace openr {

namespace {
/*
 * Histogram buckets: 1ms resolution, up to 30s
 * Larger range than BGP (10s) because KvStore sync can take 2+ seconds
 * and linkstate propagation can take 50+ seconds at scale.
 */
constexpr int64_t kBucketSizeUs = 1000; /* 1ms in microseconds */
constexpr int64_t kMinUs = 0;
constexpr int64_t kMaxUs = 30000000; /* 30s in microseconds */

thread_local ProfilerThread currentThread = ProfilerThread::KVSTORE;

} // namespace

ProfileStat::ThreadData::ThreadData()
    : histogram(kBucketSizeUs, kMinUs, kMaxUs) {}

ProfileStat::ProfileStat() = default;

folly::Histogram<int64_t>
ProfileStat::getMergedHistogram() const {
  folly::Histogram<int64_t> merged = threadData[0].histogram;
  for (size_t i = 1; i < kNumThreadTypes; ++i) {
    merged.merge(threadData[i].histogram);
  }
  return merged;
}

int64_t
ProfileStat::getMergedMaxUs() const {
  int64_t maxVal = 0;
  for (const auto& td : threadData) {
    maxVal = std::max(maxVal, td.maxUs);
  }
  return maxVal;
}

int64_t
ProfileStat::getMergedTotalUs() const {
  int64_t total = 0;
  for (const auto& td : threadData) {
    total += td.totalUs;
  }
  return total;
}

OpenrProfiler*
OpenrProfiler::getInstance() {
  static OpenrProfiler* instance = []() {
    auto* p = new OpenrProfiler();
    p->enabled_.store(FLAGS_openr_profiler_enabled, std::memory_order_relaxed);
    return p;
  }();
  return instance;
}

void
OpenrProfiler::setEnabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_relaxed);
  XLOGF(INFO, "OpenrProfiler {}", enabled ? "enabled" : "disabled");
}

void
OpenrProfiler::setCurrentThread(ProfilerThread thread) {
  currentThread = thread;
}

ProfilerThread
OpenrProfiler::getCurrentThread() const {
  return currentThread;
}

void
OpenrProfiler::setFilterRegex(const std::string& regexStr) {
  if (regexStr.empty()) {
    hasFilter_.store(false, std::memory_order_relaxed);
    {
      auto lockedRegex = filterRegex_.wlock();
      *lockedRegex = nullptr;
    }
    {
      auto lockedCache = filterCache_.wlock();
      lockedCache->clear();
    }
    XLOG(INFO, "OpenrProfiler filter cleared");
  } else {
    auto regex = std::make_unique<re2::RE2>(regexStr);
    if (!regex->ok()) {
      XLOGF(ERR, "Invalid regex '{}': {}", regexStr, regex->error());
      return;
    }

    /*
     * Pre-compute matches for all known function names.
     * This eliminates regex overhead in the hot path for known functions.
     */
    folly::F14FastMap<std::string, bool> newCache;
    {
      auto lockedStats = stats_.rlock();
      for (const auto& [name, _] : *lockedStats) {
        newCache[name] = re2::RE2::FullMatch(name, *regex);
      }
    }

    {
      auto lockedCache = filterCache_.wlock();
      *lockedCache = std::move(newCache);
    }
    {
      auto lockedRegex = filterRegex_.wlock();
      *lockedRegex = std::move(regex);
    }
    hasFilter_.store(true, std::memory_order_relaxed);
    XLOGF(INFO, "OpenrProfiler filter set to: {}", regexStr);
  }
}

bool
OpenrProfiler::matchesFilter(std::string_view name) {
  /*
   * Fast path: no filter means everything matches
   */
  if (!hasFilter_.load(std::memory_order_relaxed)) {
    return true;
  }

  /*
   * Hot path: single O(1) hash lookup in the combined cache.
   * No TOCTOU race — one lock, one map.
   */
  {
    auto lockedCache = filterCache_.rlock();
    auto it = lockedCache->find(name);
    if (it != lockedCache->end()) {
      return it->second;
    }
  }

  /*
   * Cold path: new function name seen after filter was set.
   * Run regex once and cache the result.
   */
  bool matches = false;
  {
    auto lockedRegex = filterRegex_.rlock();
    if (*lockedRegex) {
      matches = re2::RE2::FullMatch(name, **lockedRegex);
    }
  }

  {
    auto lockedCache = filterCache_.wlock();
    lockedCache->emplace(std::string(name), matches);
  }

  return matches;
}

std::shared_ptr<ProfileStat>
OpenrProfiler::getStat(std::string_view name) {
  /*
   * Hot path: check if exists with read lock (no allocation)
   */
  {
    auto lockedStats = stats_.rlock();
    auto it = lockedStats->find(name);
    if (it != lockedStats->end()) {
      return it->second;
    }
  }

  /*
   * Cold path: upgrade to write lock, allocate string key only on insert
   */
  auto lockedStats = stats_.wlock();
  auto [it, inserted] = lockedStats->try_emplace(std::string(name), nullptr);
  if (inserted) {
    it->second = std::make_shared<ProfileStat>();
  }
  return it->second;
}

void
OpenrProfiler::recordFinish(
    std::string_view name, std::chrono::nanoseconds duration) {
  auto stat = getStat(name);
  stat->count.fetch_add(1, std::memory_order_relaxed);

  int64_t us =
      std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

  /* Lock-free: each thread writes to its own histogram and max */
  auto& data = stat->getThreadData(currentThread);
  data.histogram.addValue(us);
  if (us > data.maxUs) {
    data.maxUs = us;
  }
  data.totalUs += us;
}

std::vector<OpenrProfiler::StatSummary>
OpenrProfiler::getStats() const {
  std::vector<StatSummary> result;
  auto lockedStats = stats_.rlock();
  result.reserve(lockedStats->size());

  for (const auto& [name, stat] : *lockedStats) {
    auto merged = stat->getMergedHistogram();
    auto statName = name;
    result.push_back(
        StatSummary{
            .name = std::move(statName),
            .count = stat->count.load(std::memory_order_relaxed),
            .p50Ms =
                static_cast<int64_t>(merged.getPercentileEstimate(0.5) / 1000),
            .p90Ms =
                static_cast<int64_t>(merged.getPercentileEstimate(0.9) / 1000),
            .p99Ms =
                static_cast<int64_t>(merged.getPercentileEstimate(0.99) / 1000),
            .maxMs = stat->getMergedMaxUs() / 1000,
            .totalMs = stat->getMergedTotalUs() / 1000,
        });
  }
  return result;
}

void
OpenrProfiler::clearStats() {
  auto lockedStats = stats_.wlock();
  lockedStats->clear();
  XLOG(INFO, "OpenrProfiler stats cleared");
}

void
OpenrProfiler::exportToOds() {
  if (!FLAGS_openr_profiler_export_ods) {
    return;
  }

  auto stats = getStats();
  for (const auto& s : stats) {
    /*
     * Sanitize name: "KvStore::mergePublication" -> "KvStore.mergePublication"
     * ODS keys cannot contain ::
     */
    const auto& name = s.name;
    std::string key;
    key.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
      if (i + 1 < name.size() && name[i] == ':' && name[i + 1] == ':') {
        key.push_back('.');
        ++i;
      } else {
        key.push_back(name[i]);
      }
    }

    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.count", key), s.count);
    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.p50_ms", key), s.p50Ms);
    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.p90_ms", key), s.p90Ms);
    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.p99_ms", key), s.p99Ms);
    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.max_ms", key), s.maxMs);
    fb303::fbData->setCounter(
        fmt::format("openr.profiler.{}.total_ms", key), s.totalMs);
  }
}

/*
 * ScopedProfile cold-path methods.
 * The hot path (disabled check) is inlined in the header.
 * Profiler pointer is cached from the constructor — no redundant getInstance().
 */
void
ScopedProfile::recordStart(OpenrProfiler* profiler) noexcept {
  if (profiler->hasFilter() && !profiler->matchesFilter(name_)) {
    return;
  }

  shouldRecord_ = true;
  start_ = std::chrono::steady_clock::now();
}

void
ScopedProfile::recordEnd() noexcept {
  auto end = std::chrono::steady_clock::now();
  OpenrProfiler::getInstance()->recordFinish(name_, end - start_);
}

} // namespace openr
