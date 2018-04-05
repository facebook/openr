/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrefixAllocator.h"

#include <exception>

#include <fbzmq/zmq/Zmq.h>
#include <folly/Format.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/futures/Future.h>

#include <openr/common/Constants.h>
#include <openr/common/Util.h>

using namespace fbzmq;
using namespace std::chrono_literals;

using apache::thrift::FRAGILE;

namespace {

// key for the persist config on disk
const std::string kConfigKey{"prefix-allocator-config"};

} // namespace

namespace openr {

PrefixAllocator::PrefixAllocator(
    const std::string& myNodeName,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const PrefixManagerLocalCmdUrl& prefixManagerLocalCmdUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const AllocPrefixMarker& allocPrefixMarker,
    const PrefixAllocatorMode& allocMode,
    bool setLoopbackAddress,
    bool overrideGlobalAddress,
    const std::string& loopbackIfaceName,
    std::chrono::milliseconds syncInterval,
    PersistentStoreUrl const& configStoreUrl,
    fbzmq::Context& zmqContext)
    : myNodeName_(myNodeName),
      allocPrefixMarker_(allocPrefixMarker),
      setLoopbackAddress_(setLoopbackAddress),
      overrideGlobalAddress_(overrideGlobalAddress),
      loopbackIfaceName_(loopbackIfaceName),
      syncInterval_(syncInterval),
      configStoreClient_(configStoreUrl, zmqContext),
      zmqMonitorClient_(zmqContext, monitorSubmitUrl) {

  // Create KvStore client
  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext, this, myNodeName_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl);

  // Create PrefixManager client
  prefixManagerClient_ = std::make_unique<PrefixManagerClient>(
      prefixManagerLocalCmdUrl, zmqContext);

  // Let the magic begin. Start allocation as per allocMode
  boost::apply_visitor(*this, allocMode);
}

void
PrefixAllocator::operator()(PrefixAllocatorModeStatic const&) {
  // subscribe for incremental updates of static prefix allocation key
  kvStoreClient_->subscribeKey(Constants::kStaticPrefixAllocParamKey,
    [&](std::string const& key, folly::Optional<thrift::Value> value) {
      CHECK_EQ(Constants::kStaticPrefixAllocParamKey, key);
      if (value.hasValue()){
        processStaticPrefixAllocUpdate(value.value());
      }
    }, false);

  // get initial value if missed out in incremental updates (one time only)
  scheduleTimeout(0ms, [this]() noexcept {
    // If we already have received initial value from KvStore then just skip
    // this step
    if (allocParams_.hasValue()) {
      return;
    }

    // 1) Get initial value from KvStore!
    auto maybeValue = kvStoreClient_->getKey(
        Constants::kStaticPrefixAllocParamKey);
    if (maybeValue.hasError()) {
      LOG(ERROR) << "Failed to retrieve prefix alloc params from KvStore "
                 << maybeValue.error();
    } else {
      processStaticPrefixAllocUpdate(maybeValue.value());
      return;
    }

    // 2) Start prefix allocator from previously configured params. Resume
    // from where we left earlier!
    auto maybeThriftAllocPrefix =
      configStoreClient_.loadThriftObj<thrift::AllocPrefix>(kConfigKey);
    if (maybeThriftAllocPrefix.hasValue()) {
      const auto oldAllocParams = std::make_pair(
        toIPNetwork(maybeThriftAllocPrefix->seedPrefix),
        static_cast<uint8_t>(maybeThriftAllocPrefix->allocPrefixLen));
      allocParams_ = oldAllocParams;
      applyMyPrefixIndex(maybeThriftAllocPrefix->allocPrefixIndex);
      return;
    }

    // If we weren't able to get alloc parameters so far, either from disk
    // or kvstore then let's bail out, flush out previously elected address
    // (from PrefixManager and loopback iface).
    if (!allocParams_.hasValue()) {
      LOG(WARNING)
        << "Clearing previous prefix allocation state on failure to load "
        << "allocation parameters from disk as well as KvStore.";
      applyMyPrefix(folly::none);
      return;
    }
  });
}

void
PrefixAllocator::operator()(PrefixAllocatorModeSeeded const&) {
  // subscribe for incremental updates of seed prefix
  kvStoreClient_->subscribeKey(Constants::kSeedPrefixAllocParamKey,
    [&](std::string const& key, folly::Optional<thrift::Value> value) {
      CHECK_EQ(Constants::kSeedPrefixAllocParamKey, key);
      if (value.hasValue()) {
        processAllocParamUpdate(value.value());
      }
    }, false);

  // get initial value if missed out in incremental updates (one time only)
  scheduleTimeout(0ms, [this]() noexcept {
    // If we already have received initial value from KvStore then just skip
    // this step
    if (allocParams_.hasValue()) {
      return;
    }

    // 1) Get initial value from KvStore!
    auto maybeValue = kvStoreClient_->getKey(
        Constants::kSeedPrefixAllocParamKey);
    if (maybeValue.hasError()) {
      LOG(ERROR) << "Failed to retrieve prefix alloc params from KvStore "
                 << maybeValue.error();
    } else {
      processAllocParamUpdate(maybeValue.value());
      return;
    }

    // 2) Start prefix allocator from previously configured params. Resume
    // from where we left earlier!
    auto maybeThriftAllocPrefix =
      configStoreClient_.loadThriftObj<thrift::AllocPrefix>(kConfigKey);
    if (maybeThriftAllocPrefix.hasValue()) {
      const auto oldAllocParams = std::make_pair(
        toIPNetwork(maybeThriftAllocPrefix->seedPrefix),
        static_cast<uint8_t>(maybeThriftAllocPrefix->allocPrefixLen));
      startAllocation(oldAllocParams);
      return;
    }

    // If we weren't able to get alloc parameters so far, either from disk
    // or kvstore then let's bail out and stop allocation process and
    // withdraw our prefixes from PrefixManager as well as from loopback
    // interface.
    if (!allocParams_.hasValue()) {
      LOG(WARNING)
        << "Clearing previous prefix allocation state on failure to load "
        << "allocation parameters from disk as well as KvStore.";
      applyMyPrefix(folly::none);
      return;
    }
  });
}

void
PrefixAllocator::operator()(
    PrefixAllocatorParams const& allocParams) {
  // Some sanity checks
  const auto& seedPrefix = allocParams.first;
  const auto& allocPrefixLen = allocParams.second;
  CHECK_GT(allocPrefixLen, seedPrefix.second)
    << "Allocation prefix length must be greater than seed prefix length.";
  // Start allocation from user provided seed prefix
  scheduleTimeout(0ms, [this, allocParams]() noexcept {
    startAllocation(allocParams);
  });
}

folly::Optional<uint32_t>
PrefixAllocator::getMyPrefixIndex() {
  if (isInEventLoop()) {
    return myPrefixIndex_;
  }

  // Otherwise enqueue request in eventloop and wait for result to be populated
  folly::Promise<folly::Optional<uint32_t>> promise;
  auto future = promise.getFuture();
  runInEventLoop([this, promise = std::move(promise)]() mutable {
    promise.setValue(myPrefixIndex_);
  });
  return future.get();
}

folly::Expected<PrefixAllocatorParams, fbzmq::Error>
PrefixAllocator::parseParamsStr(const std::string& paramStr) noexcept {
  // Parse string to get seed-prefix and alloc-prefix-length
  std::string seedPrefixStr;
  uint8_t allocPrefixLen;
  folly::split(
      Constants::kSeedPrefixAllocLenSeparator,
      paramStr, seedPrefixStr, allocPrefixLen);

  // Validate and convert seed-prefix to strong type, folly::CIDRNetwork
  PrefixAllocatorParams params;
  params.second = allocPrefixLen;
  try {
    params.first = folly::IPAddress::createNetwork(
        seedPrefixStr, -1 /* default mask len */);
  } catch (std::exception const& err) {
    return folly::makeUnexpected(fbzmq::Error(0, folly::sformat(
            "Invalid seed prefix {}", seedPrefixStr)));
  }

  // Validate alloc-prefix-length is larger than seed-prefix-length
  if (allocPrefixLen <= params.first.second) {
    return folly::makeUnexpected(fbzmq::Error(0, folly::sformat(
            "Seed prefix ({}) is more specific than alloc prefix len ({})",
            seedPrefixStr, allocPrefixLen)));
  }

  // Return parsed parameters
  return params;
}

void
PrefixAllocator::processStaticPrefixAllocUpdate(thrift::Value const& value) {
  CHECK(value.value.hasValue());

  // Parse thrift::Value into thrift::StaticAllocation
  thrift::StaticAllocation staticAlloc;
  try {
    staticAlloc = fbzmq::util::readThriftObjStr<thrift::StaticAllocation>(
      *value.value, serializer_);
  } catch (std::exception const& e) {
    LOG(ERROR) << "Error parsing static prefix allocation value. Error: "
               << folly::exceptionStr(e);
    if (allocParams_) {
      applyMyPrefixIndex(folly::none);
      allocParams_ = folly::none;
    }
    return;
  }
  VLOG(1) << "Processing static prefix allocation update";

  // Look for my prefix in static allocation map
  auto myPrefixIt = staticAlloc.nodePrefixes.find(myNodeName_);

  // Withdraw my prefix if not found
  if (myPrefixIt == staticAlloc.nodePrefixes.end() and allocParams_) {
    VLOG(2) << "Lost prefix";
    applyMyPrefix(folly::none);
    allocParams_ = folly::none;
  }

  // Advertise my prefix if found
  if (myPrefixIt != staticAlloc.nodePrefixes.end()) {
    VLOG(2) << "Received new prefix";
    const auto prefix = toIPNetwork(myPrefixIt->second);
    const auto newParams = std::make_pair(prefix, prefix.second);
    if (allocParams_ == newParams) {
      LOG(INFO) << "New and old params are same. Skipping";
    } else if (allocParams_.hasValue()) {
      // withdraw old prefix
      applyMyPrefixIndex(folly::none);
    }
    // create alloc params so that we share same workflow as of SEEDED mode
    // seedPrefix length is same as alloc prefix length and my prefix index is 0
    allocParams_ = std::make_pair(prefix, prefix.second); // seed prefix
    applyMyPrefixIndex(0);  // 0th index is what we own.
  }
}

void
PrefixAllocator::processAllocParamUpdate(thrift::Value const& value) {
  CHECK(value.value.hasValue());
  auto maybeParams = parseParamsStr(value.value.value());
  if (maybeParams.hasError()) {
    LOG(ERROR) << "Malformed prefix-allocator params. " << maybeParams.error();
    startAllocation(folly::none);
  } else {
    startAllocation(maybeParams.value());
  }
}

uint32_t
PrefixAllocator::getPrefixCount(
    PrefixAllocatorParams const& allocParams) noexcept {
  auto const& seedPrefix = allocParams.first;
  auto const& allocPrefixLen = allocParams.second;

  // If range of prefix alloc is greater than 32 bit integer we won't let it
  // overflow.
  return (1 << std::min(31, allocPrefixLen - seedPrefix.second));
}

folly::Optional<uint32_t>
PrefixAllocator::loadPrefixIndexFromKvStore() {
  VLOG(4) << "See if I am already allocated a prefix in kvstore";

  // This is not done at cold start, but rather some time later. So
  // probably we have synchronized with existing kv store if it is present
  return rangeAllocator_->getValueFromKvStore();
}

folly::Optional<uint32_t>
PrefixAllocator::loadPrefixIndexFromDisk() {
  auto maybeThriftAllocPrefix =
      configStoreClient_.loadThriftObj<thrift::AllocPrefix>(kConfigKey);
  if (maybeThriftAllocPrefix.hasError()) {
    return folly::none;
  }
  return static_cast<uint32_t>(maybeThriftAllocPrefix->allocPrefixIndex);
}

void
PrefixAllocator::savePrefixIndexToDisk(folly::Optional<uint32_t> prefixIndex) {
  CHECK(allocParams_.hasValue());

  if (!prefixIndex) {
    VLOG(4) << "Erasing prefix-allocator info from persistent config.";
    configStoreClient_.erase(kConfigKey);
    return;
  }

  VLOG(4) << "Saving prefix-allocator info to persistent config with index "
          << *prefixIndex;
  auto prefix = thrift::IpPrefix(
      apache::thrift::FRAGILE,
      toBinaryAddress(allocParams_->first.first),
      allocParams_->first.second);
  thrift::AllocPrefix thriftAllocPrefix(
      apache::thrift::FRAGILE,
      prefix,
      static_cast<int64_t>(allocParams_->second),
      static_cast<int64_t>(*prefixIndex));

  auto res = configStoreClient_.storeThriftObj(kConfigKey, thriftAllocPrefix);
  if (!(res.hasValue() && res.value())) {
    LOG(ERROR) << "Error saving prefix-allocator info to persistent config. "
               << res.error();
  }
}

uint32_t
PrefixAllocator::getInitPrefixIndex() {
  // initialize my prefix per the following preferrence:
  // from file > from kvstore > generate new

  // Try to get prefix index from disk
  const auto diskPrefixIndex = loadPrefixIndexFromDisk();
  if (diskPrefixIndex.hasValue()) {
    LOG(INFO) << "Got initial prefix index from disk: " << *diskPrefixIndex;
    return diskPrefixIndex.value();
  }

  // Try to get prefix index from KvStore
  const auto kvstorePrefixIndex = loadPrefixIndexFromKvStore();
  if (kvstorePrefixIndex.hasValue()) {
    LOG(INFO) << "Got initial prefix index from KvStore: "
              << *kvstorePrefixIndex;
    return kvstorePrefixIndex.value();
  }

  // Generate a new random prefix index
  if (allocParams_.hasValue()) {
    uint32_t hashPrefixIndex =
      hasher(myNodeName_) % getPrefixCount(*allocParams_);
    LOG(INFO) << "Generate new initial prefix index: " << hashPrefixIndex;
    return hashPrefixIndex;
  }

  // `0` is always a valid index in valid range
  return 0;
}

void
PrefixAllocator::startAllocation(
    folly::Optional<PrefixAllocatorParams> const& allocParams) {
  // Some informative logging
  if (allocParams_.hasValue() and allocParams.hasValue()) {
    if (allocParams_ == allocParams) {
      LOG(INFO) << "New and old params are same. Skipping";
      return;
    }
    LOG(WARNING)
      << "Prefix allocation parameters are changing. \n"
      << "  Old: " << folly::IPAddress::networkToString(allocParams_->first)
      << ", " << static_cast<int16_t>(allocParams_->second) << "\n"
      << "  New: " << folly::IPAddress::networkToString(allocParams->first)
      << ", " << static_cast<int16_t>(allocParams->second);
  }
  if (allocParams_.hasValue() and not allocParams.hasValue()) {
    LOG(WARNING) << "Prefix allocation parameters are not valid anymore. \n"
      << "  Old: " << folly::IPAddress::networkToString(allocParams_->first)
      << ", " << static_cast<int16_t>(allocParams_->second) << "\n";
  }
  if (not allocParams_.hasValue() and allocParams.hasValue()) {
    LOG(INFO)
      << "Prefix allocation parameters have been received. \n"
      << "  New: " << folly::IPAddress::networkToString(allocParams->first)
      << ", " << static_cast<int16_t>(allocParams->second);
  }
  logPrefixEvent(
      "ALLOC_PARAMS_UPDATE",
      folly::none, folly::none,
      allocParams_ /* old params */,
      allocParams /* new params */);

  // Update local state
  rangeAllocator_.reset();
  if (allocParams_) {
    applyMyPrefixIndex(folly::none);   // Clear local state
  }
  CHECK(!myPrefixIndex_.hasValue());
  allocParams_ = allocParams;

  if (!allocParams_.hasValue()) {
    return;
  }

  // create range allocator to get unique prefixes
  rangeAllocator_ = std::make_unique<RangeAllocator<uint32_t>>(
      myNodeName_,
      allocPrefixMarker_,
      kvStoreClient_.get(),
      [this](folly::Optional<uint32_t> newPrefixIndex) noexcept {
        applyMyPrefixIndex(newPrefixIndex);
      },
      syncInterval_,
      // no need for randomness since "collision" is harmless
      syncInterval_ + 1ms,
      // do not allow override
      false);

  // start range allocation
  LOG(INFO)
    << "Starting prefix allocation with seed prefix: "
    << folly::IPAddress::networkToString(allocParams_->first)
    << ", allocation prefix length: "
    << static_cast<int16_t>(allocParams_->second);
  const uint32_t prefixCount = getPrefixCount(*allocParams_);
  rangeAllocator_->startAllocator(
      std::make_pair(0, prefixCount - 1), getInitPrefixIndex());
}

void
PrefixAllocator::applyMyPrefixIndex(folly::Optional<uint32_t> prefixIndex) {
  // Silently return if nothing changed. For e.g.
  if (myPrefixIndex_ == prefixIndex) {
    return;
  }

  CHECK(allocParams_.hasValue()) << "Alloc parameters are not set.";

  // Save information to disk
  savePrefixIndexToDisk(prefixIndex);

  if (prefixIndex and !myPrefixIndex_) {
    LOG(INFO) << "Elected new prefixIndex " << *prefixIndex;
    logPrefixEvent("PREFIX_ELECTED", folly::none, prefixIndex);
  } else if (prefixIndex and myPrefixIndex_) {
    LOG(INFO) << "Updating prefixIndex to " << *prefixIndex << " from "
              << *myPrefixIndex_;
    logPrefixEvent("PREFIX_UPDATED", myPrefixIndex_, prefixIndex);
  } else if (myPrefixIndex_) {
    LOG(INFO) << "Lost previously allocated prefixIndex " << *myPrefixIndex_;
    logPrefixEvent("PREFIX_LOST", myPrefixIndex_, folly::none);
  }

  // Set local state
  myPrefixIndex_ = prefixIndex;

  // Create network prefix to announce and loopback address to assign
  folly::Optional<folly::CIDRNetwork> prefix;
  if (prefixIndex) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    prefix = getNthPrefix(seedPrefix, allocPrefixLen, *prefixIndex);
  }

  // Announce my prefix
  applyMyPrefix(std::move(prefix));
}

void
PrefixAllocator::applyMyPrefix(folly::Optional<folly::CIDRNetwork> prefix) {
  if (prefix) {
    updateMyPrefix(*prefix);
  } else {
    withdrawMyPrefix();
  }
}

void
PrefixAllocator::updateMyPrefix(folly::CIDRNetwork prefix) {
  CHECK(allocParams_.hasValue()) << "Alloc parameters are not set.";
  // existing global prefixes
  auto oldPrefixes =
      getIfacePrefixes(loopbackIfaceName_, prefix.first.family());

  // desired global prefixes
  auto loopbackPrefix = createLoopbackPrefix(prefix);
  std::vector<folly::CIDRNetwork> newPrefixes{loopbackPrefix};

  // get a list of prefixes need to be deleted
  std::vector<folly::CIDRNetwork> toDeletePrefixes;
  std::set_difference(oldPrefixes.begin(), oldPrefixes.end(),
                      newPrefixes.begin(), newPrefixes.end(),
                      std::inserter(toDeletePrefixes,
                        toDeletePrefixes.begin()));

  // delete unwanted global prefixes
  for (const auto& toDeletePrefix : toDeletePrefixes) {
    bool needToDelete = false;
    if (toDeletePrefix.first.inSubnet(
          allocParams_->first.first, allocParams_->first.second)) {
      // delete existing prefix in the subnet as seedPrefix
      needToDelete = true;
    } else if (overrideGlobalAddress_ and !toDeletePrefix.first.isLinkLocal()) {
      // delete non-link-local addresses
      needToDelete = true;
    }

    if (!needToDelete or !setLoopbackAddress_) {
      continue;
    }

    LOG(INFO) << "Delete address "
              << folly::IPAddress::networkToString(toDeletePrefix)
              << " on interface " << loopbackIfaceName_;
    if (!delIfaceAddr(loopbackIfaceName_, toDeletePrefix)) {
      LOG(FATAL) << "Failed to delete address "
                 << folly::IPAddress::networkToString(toDeletePrefix)
                 << " on interface " << loopbackIfaceName_;
    }
  }

  // Assign new address to loopback
  if (setLoopbackAddress_) {
    LOG(INFO) << "Assigning address "
              << folly::IPAddress::networkToString(loopbackPrefix)
              << " on interface " << loopbackIfaceName_;
    if (!addIfaceAddr(loopbackIfaceName_, loopbackPrefix)) {
      LOG(FATAL) << "Failed to assign address "
                 << folly::IPAddress::networkToString(loopbackPrefix)
                 << " on interface " << loopbackIfaceName_;
    }
  }

  // replace previously allocated prefix with newly allocated one
  auto ret = prefixManagerClient_->syncPrefixesByType(
      openr::thrift::PrefixType::PREFIX_ALLOCATOR,
      {openr::thrift::PrefixEntry(
          apache::thrift::FRAGILE,
          toIpPrefix(prefix),
          openr::thrift::PrefixType::PREFIX_ALLOCATOR,
          {})});
  if (ret.hasError()) {
    LOG(ERROR) << "Applying new prefix failed: " << ret.error();
  }
}

void
PrefixAllocator::withdrawMyPrefix() {
  // Flush existing loopback addresses
  if (setLoopbackAddress_ and allocParams_.hasValue()) {
    LOG(INFO) << "Flushing existing addresses from interface "
              << loopbackIfaceName_;
    if (!flushIfaceAddrs(
            loopbackIfaceName_, allocParams_->first, overrideGlobalAddress_)) {
      LOG(FATAL) << "Failed to flush addresses on interface "
                 << loopbackIfaceName_;
    }
  }

  // withdraw prefix via prefixMgrClient
  auto ret = prefixManagerClient_->withdrawPrefixesByType(
      openr::thrift::PrefixType::PREFIX_ALLOCATOR);
  if (ret.hasError()) {
    LOG(ERROR) << "Withdrawing old prefix failed: " << ret.error();
  }
}

void
PrefixAllocator::logPrefixEvent(
    std::string event,
    folly::Optional<uint32_t> oldPrefix,
    folly::Optional<uint32_t> newPrefix,
    folly::Optional<PrefixAllocatorParams> const& oldAllocParams,
    folly::Optional<PrefixAllocatorParams> const& newAllocParams) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "PrefixAllocator");
  sample.addString("node_name", myNodeName_);
  if (allocParams_.hasValue() && oldPrefix) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    sample.addString("old_prefix",
      folly::IPAddress::networkToString(
        getNthPrefix(seedPrefix, allocPrefixLen, *oldPrefix)));
  }

  if (allocParams_.hasValue() && newPrefix) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    sample.addString("new_prefix",
      folly::IPAddress::networkToString(
        getNthPrefix(seedPrefix, allocPrefixLen, *newPrefix)));
  }

  if (oldAllocParams.hasValue()) {
    sample.addString("old_seed_prefix",
        folly::IPAddress::networkToString(oldAllocParams->first));
    sample.addInt("old_alloc_len", oldAllocParams->second);
  }

  if (newAllocParams.hasValue()) {
    sample.addString("new_seed_prefix",
        folly::IPAddress::networkToString(newAllocParams->first));
    sample.addInt("new_alloc_len", newAllocParams->second);
  }

  zmqMonitorClient_.addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

} // namespace openr
