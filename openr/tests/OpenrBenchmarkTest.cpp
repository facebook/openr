/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>
#include <common/time/Time.h>
#include <folly/init/Init.h>

#include <maestro/if/OdsRouter/gen-cpp2/OdsRouter.h>
#include <maestro/if/OdsRouter/gen-cpp2/OdsRouter_types.h>
#include <maestro/if/gen-cpp2/Maestro_types.h>
#include <nettools/ebbmon/if/gen-cpp2/Exporter_types.h>
#include <openr/config-store/tests/PersistentStoreBenchmark.h>
#include <openr/fib/tests/FibBenchmark.h>
#include <openr/platform/tests/NetlinkFibHandlerBenchmark.h>
#include <servicerouter/client/cpp2/ServiceRouter.h>

DEFINE_bool(export_to_ods, false, "pass this to export data to ODS");

using Run = benchmark::BenchmarkReporter::Run;
const std::string kOdsTier = "ods_router.script";
const std::string kOdsEntity = "openr.benchmarks";

namespace openr {
// Register benchmark tests
BENCHMARK(BM_PersistentStoreWrite)->RangeMultiplier(10)->Range(10, 100000);
BENCHMARK(BM_PersistentStoreLoad)->RangeMultiplier(10)->Range(10, 100000);
BENCHMARK(BM_PersistentStoreCreateDestroy)
    ->RangeMultiplier(10)
    ->Range(10, 100000);

// Fib
BENCHMARK(BM_Fib)->RangeMultiplier(10)->Range(10, 10000);

// NetlinkFibHandler
BENCHMARK(BM_NetlinkFibHandler)->RangeMultiplier(10)->Range(10, 10000);

} // namespace openr

// Format the key sent to ODS to <bmName>.<inputSize>.<metric_<timeUnit>>.
// <optional subMetric>
static std::string
formatOdsKey(
    std::string const& bmNameInput,
    std::string const& metric,
    folly::Optional<std::string> const& subMetric) {
  // bmNameInput has the format: <bmName>/<inputSize>
  std::string::size_type pos = bmNameInput.find("/");
  const auto& bmName =
      (pos == std::string::npos) ? bmNameInput : bmNameInput.substr(0, pos);
  const auto& inputSize = (pos == std::string::npos)
      ? std::string("unknown")
      : bmNameInput.substr(pos + 1, bmNameInput.size());

  return subMetric.hasValue()
      ? folly::sformat(
            "{}.{}.{}.{}", bmName, inputSize, metric, subMetric.value())
      : folly::sformat("{}.{}.{}", bmName, inputSize, metric);
}

// Export data points to ODS
void
exportDataToOds(std::vector<Run> const& runs) {
  // Build ODS values to be submitted
  facebook::maestro::SetOdsRawValuesRequest request;
  facebook::maestro::ODSAppValue appValue;
  appValue.unixTime = facebook::WallClockUtil::NowInMsFast();
  appValue.entity = kOdsEntity;

  for (auto const& run : runs) {
    // The default time unit in benchmark is nanoseconds
    appValue.key =
        formatOdsKey(run.benchmark_name, "avg_real_time_ns", folly::none);
    appValue.value = run.GetAdjustedRealTime();
    request.dataPoints.emplace_back(appValue);
    appValue.key =
        formatOdsKey(run.benchmark_name, "avg_cpu_time_ns", folly::none);
    appValue.value = run.GetAdjustedCPUTime();
    request.dataPoints.emplace_back(appValue);

    for (auto const& counter : run.counters) {
      // The time unit in perfevent is millisecond,
      // * 1000,000 to convert it to nanoseconds
      appValue.key =
          formatOdsKey(run.benchmark_name, "avg_real_time_ns", counter.first);
      appValue.value = counter.second * 1000000;
      request.dataPoints.emplace_back(appValue);
    }
  }

  // Try to submit ODS values
  try {
    LOG(INFO) << "Sending " << request.dataPoints.size() << " datapoints";
    folly::EventBase eb;
    auto& clientFactory = facebook::servicerouter::cpp2::getClientFactory();
    auto odsclient =
        clientFactory.getClientUnique<facebook::maestro::OdsRouterAsyncClient>(
            kOdsTier, &eb);
    odsclient->sync_setOdsRawValues(request);

  } catch (std::exception const& e) {
    LOG(ERROR) << "Thrift error. " << folly::exceptionStr(e);
  }
}

// Benchmark output reporter
class TestReporter : public benchmark::ConsoleReporter {
 public:
  void
  ReportRuns(const std::vector<Run>& report) override {
    all_runs_.insert(all_runs_.end(), begin(report), end(report));
    ConsoleReporter::ReportRuns(report);
  }

  std::vector<Run> all_runs_;
};

int
main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);
  folly::init(&argc, &argv);

  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  // Run benchmark tests
  TestReporter display_reporter;
  ::benchmark::RunSpecifiedBenchmarks(&display_reporter);

  // Send data to Ods
  if (FLAGS_export_to_ods) {
    exportDataToOds(display_reporter.all_runs_);
  }
}
