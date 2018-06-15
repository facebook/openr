/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>

#include <boost/serialization/strong_typedef.hpp>
#include <fbzmq/zmq/Zmq.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>

#include <openr/common/Util.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/if/gen-cpp2/IpPrefix_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/if/gen-cpp2/PrefixManager_types.h>
#include <openr/kvstore/KvStoreClient.h>

namespace openr {

class PrefixManager final : public fbzmq::ZmqEventLoop {
 public:
  PrefixManager(
      const std::string& nodeId,
      const PrefixManagerGlobalCmdUrl& globalCmdUrl,
      const PrefixManagerLocalCmdUrl& localCmdUrl,
      const PersistentStoreUrl& persistentStoreUrl,
      const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      const PrefixDbMarker& prefixDbMarker,
      // enable convergence performance measurement for Adjacencies update
      bool enablePerfMeasurement,
      const MonitorSubmitUrl& monitorSubmitUrl,
      fbzmq::Context& zmqContext);

  // disable copying
  PrefixManager(PrefixManager const&) = delete;
  PrefixManager& operator=(PrefixManager const&) = delete;

  // get prefix add counter
  int64_t getPrefixAddCounter();

  // get prefix withdraw counter
  int64_t getPrefixWithdrawCounter();

 private:
  void persistPrefixDb();
  void processRequest(fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER>& cmdSock);

  // helpers to modify prefix db, returns true if the db is modified
  void addOrUpdatePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixes(const std::vector<thrift::PrefixEntry>& prefixes);
  bool removePrefixesByType(thrift::PrefixType type);
  // replace all prefixes of @type w/ @prefixes
  void syncPrefixesByType(
      thrift::PrefixType type,
      const std::vector<thrift::PrefixEntry>& prefixes);

  // Submit internal state counters to monitor
  void submitCounters();

  //prefix counter for a given key
  int64_t getCounter(const std::string& key);

  // this node name
  const std::string nodeId_;

  // Command Sockets to listen from requests
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> globalCmdSock_;
  fbzmq::Socket<ZMQ_ROUTER, fbzmq::ZMQ_SERVER> localCmdSock_;

  // client to interact with ConfigStore
  PersistentStoreClient configStoreClient_;

  const PrefixDbMarker prefixDbMarker_;

  // enable convergence performance measurement for Adjacencies update
  const bool enablePerfMeasurement_{false};

  // kvStoreClient for persisting our prefix db
  KvStoreClient kvStoreClient_;

  // the current prefix db this node is advertising
  std::unordered_map<thrift::IpPrefix, thrift::PrefixEntry> prefixMap_;

  // the serializer/deserializer helper we'll be using
  apache::thrift::CompactSerializer serializer_;

  // Timer for submitting to monitor periodically
  std::unique_ptr<fbzmq::ZmqTimeout> monitorTimer_{nullptr};

  // DS to keep track of stats
  fbzmq::ThreadData tData_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

}; // PrefixManager

} // namespace openr
