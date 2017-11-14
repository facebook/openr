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
      // prefix to allocate prefixes from
      const folly::Optional<folly::CIDRNetwork> seedPrefix,
      // allocated prefix length
      uint32_t allocPrefixLen,
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

  // check if all prefixes are allocated (must be called on same eventloop)
  bool allPrefixAllocated();

 private:
  //
  // Private methods
  //

  // get my existing prefix index from kvstore if it's present
  folly::Optional<uint32_t> getMyPrefixIndexFromKvStore();

  // initialize my prefix
  void initMyPrefix();

  // load prefix from disk
  folly::Optional<uint32_t> loadPrefixFromDisk() const;

  // save newly elected prefix to disk
  void savePrefixToDisk(folly::Optional<uint32_t> prefixIndex);

  // use my newly allocated prefix
  void applyMyPrefix(folly::Optional<uint32_t> prefixIndex);

  //
  // Private variables
  //
  // get key value if exists
  folly::Optional<std::string> getValueByKey(
      const std::string& keyName) noexcept;

  // check for seed prefix
  void checkSeedPrefix();

  // start allocating prefixes
  void startAlloc();

  void logPrefixEvent(
      std::string event,
      folly::Optional<uint32_t> oldPrefix,
      folly::Optional<uint32_t> newPrefix);

  // this node's name
  const std::string myNodeName_{};

  // this node's key marker for prefix allocation
  const std::string allocPrefixMarker_{};

  // prefix to allocate prefixes from, e.g., fc00:cafe::/56
  folly::Optional<folly::CIDRNetwork> seedPrefix_{};

  // prefix size, e.g., 64 in fc00:cafe::/64
  uint32_t allocPrefixLen_{0};

  // Parameter to set loopback addresses
  const bool setLoopbackAddress_{false};
  const bool overrideGlobalAddress_{false};
  const std::string loopbackIfaceName_;

  // total number of available prefixes in seed prefix
  uint32_t prefixCount_{0};

  // index of my currently claimed prefix within seed prefix
  folly::Optional<uint32_t> myPrefixIndex_;

  // hash node ID into prefix space
  std::hash<std::string> hasher{};

  apache::thrift::CompactSerializer serializer_;

  // periodic timer to check for a seed prefix from KvStore
  // Used if the seed prefix was not already supplied to us
  std::unique_ptr<fbzmq::ZmqTimeout> checkSeedPrefixTimer_{nullptr};

  // interval to run sync with
  // use ms, not seconds as in KvStore, mainly to save unit test time
  const std::chrono::milliseconds syncInterval_{0};

  // we'll use this to get the full dump from the KvStore
  // and get and set my assigned prefix
  std::unique_ptr<KvStoreClient> kvStoreClient_{nullptr};

  // client to interact with ConfigStore
  PersistentStoreClient configStoreClient_;

  // RangAlloctor to get unique prefix index for this node
  std::unique_ptr<RangeAllocator<uint32_t>> rangeAllocator_;

  // PrefixManager client
  std::unique_ptr<PrefixManagerClient> prefixManagerClient_;

  fbzmq::ZmqMonitorClient zmqMonitorClient_;
};

} // namespace openr
