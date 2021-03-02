/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/allocators/RangeAllocator.h>
#include <openr/common/Constants.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/config-store/PersistentStore.h>
#include <openr/config/Config.h>
#include <openr/if/gen-cpp2/Types_types.h>
#include <openr/kvstore/KvStore.h>
#include <openr/kvstore/KvStoreClientInternal.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/monitor/LogSample.h>
#include <openr/nl/NetlinkProtocolSocket.h>

namespace openr {
/**
 * The class assigns local node unique prefixes from a given seed prefix in
 * a distributed manner.
 *
 */
class PrefixAllocator : public OpenrEventBase {
 public:
  PrefixAllocator(
      AreaId const& area,
      std::shared_ptr<const Config> config,
      // raw ptr for modules
      fbnl::NetlinkProtocolSocket* nlSock,
      KvStore* kvStore,
      PersistentStore* configStore,
      // producer queue
      messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQ,
      messaging::ReplicateQueue<LogSample>& logSampleQueue,
      std::chrono::milliseconds syncInterval);

  PrefixAllocator(PrefixAllocator const&) = delete;
  PrefixAllocator& operator=(PrefixAllocator const&) = delete;

  /**
   * Override stop method of OpenrEventBase
   */
  void stop() override;

  // Thread safe API for testing only
  std::optional<uint32_t> getMyPrefixIndex();

  // Static function to parse string representation of allocation params to
  // strong types. Throw exception upon parsing error
  static PrefixAllocationParams parseParamsStr(const std::string& paramStr);

  // Static function to get available prefix count from allocation params
  static uint32_t getPrefixCount(
      PrefixAllocationParams const& allocParams) noexcept;

  /*
   * [Netlink Platform] util functions to add/del iface address
   */
  folly::SemiFuture<folly::Unit> semifuture_syncIfAddrs(
      std::string iface,
      int16_t family,
      int16_t scope,
      std::vector<folly::CIDRNetwork> newAddrs);

  folly::SemiFuture<folly::Unit> semifuture_addRemoveIfAddr(
      const bool isAdd,
      const std::string& ifName,
      const std::vector<folly::CIDRNetwork>& networks);

  folly::SemiFuture<std::vector<folly::CIDRNetwork>> semifuture_getIfAddrs(
      std::string ifName, int16_t family, int16_t scope);

 private:
  // 3 different ways to initialize PrefixAllocator.
  void staticAllocation();
  void dynamicAllocationLeafNode();
  void dynamicAllocationRootNode(PrefixAllocationParams const&);

  // Function to process static allocation update from kvstore
  void processStaticPrefixAllocUpdate(thrift::Value const& value);

  //  Function to process allocation param update from kvstore
  void processAllocParamUpdate(thrift::Value const& value);

  // calculate and save alloc index obtained from e2e-network-allocation
  void processNetworkAllocationsUpdate(thrift::Value const& value);

  // check if index is already in use by e2e-network-allocations
  bool checkE2eAllocIndex(uint32_t index);

  // get my existing prefix index from kvstore if it's present
  std::optional<uint32_t> loadPrefixIndexFromKvStore();

  // load prefix index from disk
  std::optional<uint32_t> loadPrefixIndexFromDisk();

  // save newly elected prefix index to disk
  void savePrefixIndexToDisk(std::optional<uint32_t> prefixIndex);

  // initialize my prefix
  uint32_t getInitPrefixIndex();

  // start allocating prefixes, can be called again with new prefix
  // or `std::nullopt` if seed prefix is no longer valid to withdraw
  // what we had before!
  void startAllocation(
      std::optional<PrefixAllocationParams> const& allocParams,
      bool checkParams = true);

  // use my newly allocated prefix
  void applyMyPrefixIndex(std::optional<uint32_t> prefixIndex);
  void applyMyPrefix();

  // update prefix
  void updateMyPrefix(folly::CIDRNetwork prefix);

  // withdraw prefix
  void withdrawMyPrefix();

  void logPrefixEvent(
      std::string event,
      std::optional<uint32_t> oldPrefix,
      std::optional<uint32_t> newPrefix,
      std::optional<PrefixAllocationParams> const& oldParams = std::nullopt,
      std::optional<PrefixAllocationParams> const& newParams = std::nullopt);

  /*
   * Synchronous API to query interface index from kernel.
   * NOTE: We intentionally don't use cache to optimize this call as APIs of
   * this handlers for add/remove/sync addresses are rarely invoked.
   */
  std::optional<int> getIfIndex(const std::string& ifName);

  //
  // Const private variables
  //

  // this node's name
  const std::string myNodeName_{};

  // Sync interval for range allocator
  const std::chrono::milliseconds syncInterval_;

  // hash node ID into prefix space
  const std::hash<std::string> hasher{};

  //
  // Non-const private variables
  //

  // Parameter to set loopback addresses
  bool setLoopbackAddress_{false};
  bool overrideGlobalAddress_{false};
  std::string loopbackIfaceName_;
  thrift::PrefixForwardingType prefixForwardingType_;
  thrift::PrefixForwardingAlgorithm prefixForwardingAlgorithm_;

  // area in which to allocate prefix
  AreaId area_;

  // Allocation parameters e.g., fc00:cafe::/56, 64
  std::optional<PrefixAllocationParams> allocParams_;

  // index of my currently claimed prefix within seed prefix
  std::optional<uint32_t> myPrefixIndex_;

  apache::thrift::CompactSerializer serializer_;

  // we'll use this to get the full dump from the KvStore
  // and get and set my assigned prefix
  std::unique_ptr<KvStoreClientInternal> kvStoreClient_{nullptr};

  // raw ptr for netlinkProtocolSocket for addr add/del
  fbnl::NetlinkProtocolSocket* nlSock_{nullptr};

  // module ptr to interact with ConfigStore
  PersistentStore* configStore_{nullptr};

  // RangAlloctor to get unique prefix index for this node
  std::unique_ptr<RangeAllocator<uint32_t>> rangeAllocator_;

  // Queue to send prefix event to PrefixManager
  messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue_;

  // Queue to publish the event log
  messaging::ReplicateQueue<LogSample>& logSampleQueue_;

  // AsyncTimeout for initialization
  std::unique_ptr<folly::AsyncTimeout> initTimer_;

  // AsyncTimeout for prefix allocation retry
  std::unique_ptr<folly::AsyncTimeout> retryTimer_;

  /**
   * applyMyPrefix use this state to decide how to program address to kernel
   * boolean field means the address is beed applied or not.
   * When Optional value is empty, it means cleanup addresses on the iface
   * otherwise applys the Optional value to the iface
   */
  std::pair<bool, std::optional<folly::CIDRNetwork>> applyState_;

  // save alloc index from e2e-network-alllocation <value version, indices set>
  std::pair<int64_t, std::unordered_set<uint32_t>> e2eAllocIndex_{-1, {}};
};

} // namespace openr
