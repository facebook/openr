/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@hack.NamePrefix{prefix = "Openr_"}
@hack.LegacyAlwaysIncludeNamePrefixInProcessor
@hack.LegacyOmitPrefixInNameString
package "meta.com/openr"

namespace hack ""

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace go openr.Health
namespace py openr.Health
namespace py3 openr.thrift
namespace lua openr.Health
namespace rust openr_thrift
namespace wiki Open_Routing.Thrift_APIs.Health

include "thrift/annotation/hack.thrift"

/**
 * Stable identifiers for the Open/R execution domains participating in health
 * report collection. Control-server EventBases are intentionally excluded.
 */
enum HealthModuleId {
  UNKNOWN = 0,
  NETLINK = 1,
  PERSISTENT_STORE = 2,
  MONITOR = 3,
  KVSTORE = 4,
  DISPATCHER = 5,
  PREFIX_MANAGER = 6,
  NEIGHBOR_MONITOR = 7,
  SPARK = 8,
  LINK_MONITOR = 9,
  DECISION = 10,
  FIB = 11,
}

/**
 * Result of a health check. UNKNOWN means no result is available, PASS means
 * the invariant was satisfied, FAIL means it was violated or could not be
 * evaluated, SKIPPED means it was not applicable, and WARN means it completed
 * with a degraded condition.
 *
 * Aggregate statuses use the precedence FAIL, UNKNOWN, WARN, PASS, SKIPPED.
 * Enum ordinals do not encode this precedence; aggregators must compare
 * statuses explicitly. An empty check list aggregates to UNKNOWN.
 */
enum HealthCheckStatus {
  UNKNOWN = 0,
  PASS = 1,
  FAIL = 2,
  SKIPPED = 3,
  WARN = 4,
}

/**
 * Stable identifier for a health check.
 *
 * When a check uses numeric fields, its identifier must document the metric,
 * unit, scale, and threshold direction. Once published, values must not be
 * renumbered or reused.
 */
enum HealthCheckId {
  UNKNOWN = 0,
}

/**
 * Result of one health check.
 */
struct HealthCheckResult {
  1: HealthCheckId checkId;
  2: HealthCheckStatus status;
  3: string message;
  /**
   * Numeric measurement used by this check. The documentation for checkId
   * defines its metric, unit, and scale. For example, 12 means a queue depth
   * of 12 entries, while 0.75 means 75% for a utilization-ratio check.
   * Unset when the check has no scalar measurement.
   */
  4: optional double observedValue;
  /**
   * Numeric boundary used to classify this result, in the same unit and scale
   * as observedValue. The documentation for checkId defines whether it is an
   * upper or lower bound. For example, 0.80 means an 80% utilization limit.
   * Unset when the check has no scalar threshold.
   */
  5: optional double threshold;
  /**
   * Unix epoch time in milliseconds when this check result was captured,
   * e.g. 1717000000000.
   */
  6: i64 timestampMs;
}

/**
 * Health report for one Open/R module.
 */
struct HealthModuleReport {
  1: HealthModuleId moduleId;
  2: list<HealthCheckResult> checks;
  3: HealthCheckStatus overallStatus;
}

/**
 * Health report for one Open/R pipe.
 */
struct HealthPipeReport {
  /**
   * Watchdog/fb303 queue name, e.g. "routeUpdatesQueue".
   */
  1: string pipeName;
  2: list<HealthCheckResult> checks;
  3: HealthCheckStatus overallStatus;
}

/**
 * Top-level Open/R health report. overallStatus summarizes every check
 * implemented by this Open/R version; PASS does not imply that future or
 * unimplemented health invariants were evaluated.
 */
struct HealthReport {
  1: list<HealthModuleReport> modules;
  2: list<HealthPipeReport> pipes;
  3: HealthCheckStatus overallStatus;
  /**
   * Unix epoch time in milliseconds when report generation began,
   * e.g. 1717000000000.
   */
  4: i64 timestampMs;
}

/**
 * Request for the canonical Open/R health report. It is intentionally empty
 * so future additive collection options do not require changing the RPC
 * signature. Report membership, safety deadlines, and health semantics remain
 * server-defined.
 */
struct HealthReportRequest {}
