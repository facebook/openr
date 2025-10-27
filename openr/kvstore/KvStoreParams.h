/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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
  // TLS paths
  std::optional<std::string> x509_cert_path{std::nullopt};
  std::optional<std::string> x509_key_path{std::nullopt};
  std::optional<std::string> x509_ca_path{std::nullopt};

  KvStoreParams(
      const thrift::KvStoreConfig& kvStoreConfig,
      messaging::ReplicateQueue<KvStorePublication>& kvStoreUpdatesQueue,
      messaging::ReplicateQueue<LogSample>& logSampleQueue)
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
        x509_cert_path(kvStoreConfig.x509_cert_path().to_optional()),
        x509_key_path(kvStoreConfig.x509_key_path().to_optional()),
        x509_ca_path(kvStoreConfig.x509_ca_path().to_optional()) {}
};

} // namespace openr
