/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <fbzmq/async/ZmqEventLoop.h>
#include <fbzmq/async/ZmqTimeout.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <folly/IPAddress.h>
#include <folly/Optional.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/AddressUtil.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStoreClient.h>
#include <openr/if/gen-cpp2/KvStore_types.h>
#include <openr/if/gen-cpp2/Lsdb_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreClient.h>
#include <openr/prefix-manager/PrefixManagerClient.h>
#include "RangeAllocator.h"

namespace openr {

using PrefixAllocatorParams = std::pair<folly::CIDRNetwork, uint8_t>;

/**
 * The class assigns local node unique prefixes from a given seed prefix in
 * a distributed manner.
 */
class PrefixAllocator : public fbzmq::ZmqEventLoop {
 public:
  PrefixAllocator(
      const std::string& myNodeName,
      const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
      const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
      const PrefixManagerLocalCmdUrl& prefixManagerLocalCmdUrl,
      const MonitorSubmitUrl& monitorSubmitUrl,
      const AllocPrefixMarker& allocPrefixMarker,
      // Allocation params
      const folly::Optional<PrefixAllocatorParams>& allocatorParams,
      // configure loopback address or not
      bool setLoopbackAddress,
      // override all global addresses on loopback interface
      bool overrideGlobalAddress,
      // loopback interface name
      const std::string& loopbackIfaceName,
      // period to check prefix collision
      std::chrono::milliseconds syncInterval,
      PersistentStoreUrl const& configStoreUrl,
      fbzmq::Context& zmqContext);

  PrefixAllocator(PrefixAllocator const&) = delete;
  PrefixAllocator& operator=(PrefixAllocator const&) = delete;

  // Thread safe API for testing only
  folly::Optional<uint32_t> getMyPrefixIndex();

  // Static function to parse string representation of allocation params to
  // strong types.
  static folly::Expected<PrefixAllocatorParams, fbzmq::Error> parseParamsStr(
      const std::string& paramStr) noexcept;

  // Static function to get available prefix count from allocation params
  static uint32_t getPrefixCount(
      PrefixAllocatorParams const& allocParams) noexcept;

 private:
  //
  // Private methods
  //

  //  Function to process param update from kvstore
  void processAllocParamUpdate(thrift::Value const& value);

  // get my existing prefix index from kvstore if it's present
  folly::Optional<uint32_t> loadPrefixIndexFromKvStore();

  // load prefix index from disk
  folly::Optional<uint32_t> loadPrefixIndexFromDisk();

  // save newly elected prefix index to disk
  void savePrefixIndexToDisk(folly::Optional<uint32_t> prefixIndex);

  // initialize my prefix
  uint32_t getInitPrefixIndex();

  // start allocating prefixes, can be called again with new prefix
  void startAllocation(PrefixAllocatorParams const& allocParams);

  // use my newly allocated prefix
  void applyMyPrefix(folly::Optional<uint32_t> prefixIndex);

  void logPrefixEvent(
      std::string event,
      folly::Optional<uint32_t> oldPrefix,
      folly::Optional<uint32_t> newPrefix,
      folly::Optional<PrefixAllocatorParams> const& oldParams = folly::none,
      folly::Optional<PrefixAllocatorParams> const& newParams = folly::none);

  //
  // Const private variables
  //

  // this node's name
  const std::string myNodeName_{};

  // this node's key marker for prefix allocation
  const std::string allocPrefixMarker_{};

  // Parameter to set loopback addresses
  const bool setLoopbackAddress_{false};
  const bool overrideGlobalAddress_{false};
  const std::string loopbackIfaceName_;

  // Sync interval for range allocator
  const std::chrono::milliseconds syncInterval_;

  // hash node ID into prefix space
  const std::hash<std::string> hasher{};

  //
  // Non-const private variables
  //

  // Allocation parameters e.g., fc00:cafe::/56, 64
  folly::Optional<PrefixAllocatorParams> allocParams_;

  // index of my currently claimed prefix within seed prefix
  folly::Optional<uint32_t> myPrefixIndex_;

  apache::thrift::CompactSerializer serializer_;

  // we'll use this to get the full dump from the KvStore
  // and get and set my assigned prefix
  std::unique_ptr<KvStoreClient> kvStoreClient_{nullptr};

  // client to interact with ConfigStore
  PersistentStoreClient configStoreClient_;

  // RangAlloctor to get unique prefix index for this node
  std::unique_ptr<RangeAllocator<uint32_t>> rangeAllocator_;

  // PrefixManager client
  std::unique_ptr<PrefixManagerClient> prefixManagerClient_;

  // Monitor client for submitting counters/logs
  fbzmq::ZmqMonitorClient zmqMonitorClient_;
};

} // namespace openr
