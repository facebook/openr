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

// seed prefix and allocated prefix length is separate by comma
const std::string kSeedPrefixAllocLenSeparator = ",";
// key of prefix allocator parameters in kv store
const std::string kPrefixAllocParamKey{"e2e-network-prefix"};
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
    const folly::Optional<PrefixAllocatorParams>& allocatorParams,
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

  // Some sanity checks
  if (allocatorParams.hasValue()) {
    const auto& seedPrefix = allocatorParams->first;
    const auto& allocPrefixLen = allocatorParams->second;
    CHECK_GT(allocPrefixLen, seedPrefix.second)
      << "Allocation prefix length must be greater than seed prefix length.";
  }

  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext, this, myNodeName_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl);

  prefixManagerClient_ = std::make_unique<PrefixManagerClient>(
      prefixManagerLocalCmdUrl, zmqContext);

  // If allocator params is not specified then look for allocator params in
  // KvStore else use user provided seed prefix.
  if (not allocatorParams.hasValue()) {
    // subscribe for incremental updates
    kvStoreClient_->subscribeKey(kPrefixAllocParamKey,
      [&](std::string const& key, thrift::Value const& value) {
        CHECK_EQ(kPrefixAllocParamKey, key);
        processAllocParamUpdate(value);
      });
    // get initial value if missed out in incremental updates (one time only)
    scheduleTimeout(0ms, [this]() noexcept {
      auto maybeValue = kvStoreClient_->getKey(kPrefixAllocParamKey);
      if (maybeValue.hasError()) {
        LOG(ERROR) << "Failed to retrieve prefix alloc params from KvStore "
                   << maybeValue.error();
      } else {
        processAllocParamUpdate(maybeValue.value());
      }
    });
  } else {
    // Start allocation from user provided seed prefix
    scheduleTimeout(0ms, [this, allocatorParams]() noexcept {
      startAllocation(*allocatorParams);
    });
  }
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
      kSeedPrefixAllocLenSeparator, paramStr, seedPrefixStr, allocPrefixLen);

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
PrefixAllocator::processAllocParamUpdate(thrift::Value const& value) {
  CHECK(value.value.hasValue());
  auto maybeParams = parseParamsStr(value.value.value());
  if (maybeParams.hasError()) {
    LOG(ERROR) << "Malformed prefix-allocator params. " << maybeParams.error();
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
  auto thriftAllocPrefix = maybeThriftAllocPrefix.value();
  return static_cast<uint32_t>(thriftAllocPrefix.allocPrefixIndex);
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
PrefixAllocator::startAllocation(PrefixAllocatorParams const& allocParams) {
  if (allocParams_.hasValue()) {
    LOG(WARNING)
      << "Prefix allocation parameters are changing. \n"
      << "  Old: " << folly::IPAddress::networkToString(allocParams_->first)
      << ", " << static_cast<int16_t>(allocParams_->second) << "\n"
      << "  New: " << folly::IPAddress::networkToString(allocParams.first)
      << ", " << static_cast<int16_t>(allocParams.second);
  }
  logPrefixEvent(
      "ALLOC_PARAMS_UPDATE",
      folly::none, folly::none,
      allocParams_ /* old params */,
      allocParams /* new params */);

  // Update local state
  rangeAllocator_.reset();
  applyMyPrefix(folly::none);   // Clear local state
  CHECK(!myPrefixIndex_.hasValue());
  allocParams_ = allocParams;

  // create range allocator to get unique prefixes
  rangeAllocator_ = std::make_unique<RangeAllocator<uint32_t>>(
      myNodeName_,
      allocPrefixMarker_,
      kvStoreClient_.get(),
      [this](folly::Optional<uint32_t> newPrefixIndex) noexcept {
        applyMyPrefix(newPrefixIndex);
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
PrefixAllocator::applyMyPrefix(folly::Optional<uint32_t> prefixIndex) {
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
  auto const& seedPrefix = allocParams_->first;
  auto const& allocPrefixLen = allocParams_->second;
  folly::Optional<folly::CIDRNetwork> prefix;
  if (prefixIndex) {
    prefix = getNthPrefix(seedPrefix, allocPrefixLen, *prefixIndex, true);
  }

  // Flush existing loopback addresses
  if (setLoopbackAddress_) {
    LOG(INFO) << "Flushing existing addresses from interface "
              << loopbackIfaceName_;
    if (!flushIfaceAddrs(
            loopbackIfaceName_, seedPrefix, overrideGlobalAddress_)) {
      LOG(FATAL) << "Failed to flush addresses on interface "
                 << loopbackIfaceName_;
    }
  }

  // Assign new address to loopback
  if (setLoopbackAddress_ and prefix) {
    auto loopbackAddr = createLoopbackAddr(*prefix);
    LOG(INFO) << "Assigning address " << loopbackAddr.str() << " on interface "
              << loopbackIfaceName_;
    if (!addIfaceAddr(loopbackIfaceName_, loopbackAddr)) {
      LOG(FATAL) << "Failed to assign address " << loopbackAddr.str()
                 << " on interface " << loopbackIfaceName_;
    }
  }

  // Announce information in PrefixManager
  LOG(INFO) << "Syncing prefix information to PrefixManager.";
  if (prefix) {
    // replace previously allocated prefix with newly allocated one
    auto ret = prefixManagerClient_->syncPrefixesByType(
        openr::thrift::PrefixType::PREFIX_ALLOCATOR,
        {openr::thrift::PrefixEntry(
            apache::thrift::FRAGILE,
            toIpPrefix(*prefix),
            openr::thrift::PrefixType::PREFIX_ALLOCATOR,
            {})});
    if (ret.hasError()) {
      LOG(ERROR) << "Applying new prefix failed: " << ret.error();
    }
  } else {
    auto ret = prefixManagerClient_->withdrawPrefixesByType(
        openr::thrift::PrefixType::PREFIX_ALLOCATOR);
    if (ret.hasError()) {
      LOG(ERROR) << "Withdrawing old prefix failed: " << ret.error();
    }
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
        getNthPrefix(seedPrefix, allocPrefixLen, *oldPrefix, true)));
  }

  if (allocParams_.hasValue() && newPrefix) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    sample.addString("new_prefix",
      folly::IPAddress::networkToString(
        getNthPrefix(seedPrefix, allocPrefixLen, *newPrefix, true)));
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
