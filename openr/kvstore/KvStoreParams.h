/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/logging/xlog.h>

#include <openr/config/Config.h>
#include <openr/kvstore/KvStoreUtil.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>

namespace openr {

/*
 * This is the structure used to convey all of the necessary information from
 * KvStore to individual KvStoreDbs(per area). This includes commonly shared
 * data structures like queues and config knobs shared across KvStoreDbs.
 */
struct KvStoreParams {
  /*
   * Resolve the flood-budget knobs, applying the Constants fallback and
   * rejecting values that would defeat the budget.
   *
   * Config::checkKvStoreConfig already rejects these at startup, but only for
   * configs built through Config. KvStoreParams is also constructed directly
   * from a thrift::KvStoreConfig by tests and by direct embedders (see
   * KvStoreWrapper), which bypasses that check entirely -- so the invariant is
   * re-established here rather than assumed.
   *
   * A negative budget is the dangerous one: the field is i64 and the member is
   * size_t, so a plain static_cast turns -1 into SIZE_MAX and silently
   * disables the bound altogether -- the exact opposite of the knob's purpose,
   * and invisible at runtime. Zero is the mirror failure: every publication
   * defers and every drain early-returns, latching flooding off.
   *
   * These log and fall back rather than throw: KvStoreParams has no throwing
   * contract, and the loud failure already exists at the Config layer for
   * anything production-facing.
   */
  static size_t
  resolveFloodMemBudgetBytes(const thrift::KvStoreConfig& kvStoreConfig) {
    auto configured = kvStoreConfig.flood_mem_budget_bytes();
    if (!configured) {
      return Constants::kFloodMemBudgetBytes;
    }
    if (*configured <= 0) {
      XLOGF(
          ERR,
          "Ignoring non-positive kvstore flood_mem_budget_bytes {}; falling back to {}",
          *configured,
          Constants::kFloodMemBudgetBytes);
      return Constants::kFloodMemBudgetBytes;
    }
    return static_cast<size_t>(*configured);
  }

  static std::chrono::milliseconds
  resolveFloodDrainReconcileThreshold(
      const thrift::KvStoreConfig& kvStoreConfig) {
    auto configured = kvStoreConfig.flood_drain_reconcile_threshold_ms();
    if (!configured) {
      return Constants::kFloodDrainReconcileThreshold;
    }
    if (*configured < Constants::kMinFloodDrainReconcileThreshold.count()) {
      XLOGF(
          ERR,
          "Ignoring kvstore flood_drain_reconcile_threshold_ms {} below the {}ms floor; falling back to {}ms",
          *configured,
          Constants::kMinFloodDrainReconcileThreshold.count(),
          Constants::kFloodDrainReconcileThreshold.count());
      return Constants::kFloodDrainReconcileThreshold;
    }
    return std::chrono::milliseconds(*configured);
  }

  // the name of this node (unique in domain)
  std::string nodeId{};

  // Queue for publishing KvStore updates to other modules within a process
  messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQueue;

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue;

  // IP ToS
  std::optional<int> maybeIpTos{std::nullopt};
  // KvStore key filters
  std::optional<KvStoreFilters> filters{std::nullopt};
  // Kvstore flooding rate
  std::optional<thrift::KvStoreFloodRate> floodRate{std::nullopt};
  // TTL decrement factor
  std::chrono::milliseconds ttlDecr{Constants::kTtlDecrement};
  // TTL for self-originated keys
  std::chrono::milliseconds keyTtl{0};
  std::chrono::milliseconds syncInitialBackoff{
      Constants::kKvstoreSyncInitialBackoff};
  std::chrono::milliseconds syncMaxBackoff{Constants::kKvstoreSyncMaxBackoff};
  // Locally adjacency learning timeout
  std::chrono::milliseconds selfAdjSyncTimeout;

  std::chrono::milliseconds kvStoreSyncTimeout;

  // TLS knob
  bool enable_secure_thrift_client{false};
  // Pre-compress flood-publication payload once, shared across peers
  bool enable_flood_pub_pre_compression{false};
  /*
   * Per-area soft budget (bytes) for in-flight flood-publication payloads, and
   * how long deferred keys may sit before a lost flood-RPC completion is
   * assumed. Config-driven, falling back to the Constants defaults, so the
   * budget can be retuned without a binary push and so tests can drive the
   * backpressure path with a small budget instead of generating a real one.
   */
  size_t floodMemBudgetBytes{Constants::kFloodMemBudgetBytes};
  std::chrono::milliseconds floodDrainReconcileThreshold{
      Constants::kFloodDrainReconcileThreshold};
  // TLS paths
  std::optional<std::string> x509_cert_path{std::nullopt};
  std::optional<std::string> x509_key_path{std::nullopt};
  std::optional<std::string> x509_ca_path{std::nullopt};

  std::optional<FabricConfig> fabricConfig;

  KvStoreParams(
      const thrift::KvStoreConfig& kvStoreConfig,
      messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      std::optional<FabricConfig> fabricConfig = std::nullopt)
      : nodeId(*kvStoreConfig.node_name()),
        kvStoreUpdatesQueue(kvStoreUpdatesQueue),
        logSampleQueue(logSampleQueue),
        floodRate(kvStoreConfig.flood_rate().to_optional()), /* Kvstore
                                                                flooding rate
                                                              */
        ttlDecr(
            std::chrono::milliseconds(
                *kvStoreConfig.ttl_decrement_ms())), /* TTL decrement factor */
        keyTtl(
            std::chrono::milliseconds(
                *kvStoreConfig.key_ttl_ms())), /*TTL for self-originated keys */
        syncInitialBackoff(
            std::chrono::milliseconds(
                *kvStoreConfig.sync_initial_backoff_ms())),
        syncMaxBackoff(
            std::chrono::milliseconds(*kvStoreConfig.sync_max_backoff_ms())),
        enable_secure_thrift_client(
            *kvStoreConfig.enable_secure_thrift_client()),
        enable_flood_pub_pre_compression(
            kvStoreConfig.enable_flood_pub_pre_compression().value_or(false)),
        floodMemBudgetBytes(resolveFloodMemBudgetBytes(kvStoreConfig)),
        floodDrainReconcileThreshold(
            resolveFloodDrainReconcileThreshold(kvStoreConfig)),
        x509_cert_path(kvStoreConfig.x509_cert_path().to_optional()),
        x509_key_path(kvStoreConfig.x509_key_path().to_optional()),
        x509_ca_path(kvStoreConfig.x509_ca_path().to_optional()),
        fabricConfig(std::move(fabricConfig)) {}
};

} // namespace openr
