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
// how often to check for seed prefix in kvstore
const std::chrono::milliseconds kCheckSeedPrefixInterval{1000};
// key of prefix allocator parameters in kv store
const std::string kPrefixAllocParamKey{"e2e-network-prefix"};
// key for the persist config on disk
const std::string kConfigKey{"prefix-allocator-config"};

bool
isSeedPrefixValid(const folly::CIDRNetwork& prefix, uint32_t allocPrefixLen) {
  if (!prefix.first.isV6()) {
    LOG(ERROR) << "Seed prefix is not IP V6";
    return false;
  }
  if (allocPrefixLen < prefix.second) {
    LOG(ERROR) << "Allocated prefix length is smaller than that of seed prefix";
    return false;
  }

  // make sure prefixCount_ can be accommodated in uint32_t
  if ((allocPrefixLen - prefix.second) >= 32) {
    // allocated prefix length
    LOG(ERROR) << "Allocated subprefix size too large, try smaller seed prefix "
                  "or longer allocated prefix";
    return false;
  }
  return true;
}
} // namespace

namespace openr {

PrefixAllocator::PrefixAllocator(
    const std::string& myNodeName,
    const KvStoreLocalCmdUrl& kvStoreLocalCmdUrl,
    const KvStoreLocalPubUrl& kvStoreLocalPubUrl,
    const PrefixManagerLocalCmdUrl& prefixManagerLocalCmdUrl,
    const MonitorSubmitUrl& monitorSubmitUrl,
    const AllocPrefixMarker& allocPrefixMarker,
    const folly::Optional<folly::CIDRNetwork> seedPrefix,
    uint32_t allocPrefixLen,
    bool setLoopbackAddress,
    bool overrideGlobalAddress,
    const std::string& loopbackIfaceName,
    std::chrono::milliseconds syncInterval,
    PersistentStoreUrl const& configStoreUrl,
    fbzmq::Context& zmqContext)
    : myNodeName_(myNodeName),
      allocPrefixMarker_(allocPrefixMarker),
      seedPrefix_(seedPrefix),
      allocPrefixLen_(allocPrefixLen),
      setLoopbackAddress_(setLoopbackAddress),
      overrideGlobalAddress_(overrideGlobalAddress),
      loopbackIfaceName_(loopbackIfaceName),
      syncInterval_(syncInterval),
      configStoreClient_(configStoreUrl, zmqContext),
      zmqMonitorClient_(zmqContext, monitorSubmitUrl) {

  kvStoreClient_ = std::make_unique<KvStoreClient>(
      zmqContext, this, myNodeName_, kvStoreLocalCmdUrl, kvStoreLocalPubUrl);

  prefixManagerClient_ = std::make_unique<PrefixManagerClient>(
      prefixManagerLocalCmdUrl, zmqContext);

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

  if (!seedPrefix_) {
    // listen to KvStore for seed prefix
    // schedule periodic timer
    // Only if we successfully get a prefix, we cancel this
    checkSeedPrefixTimer_ =
        fbzmq::ZmqTimeout::make(this, [this]() { checkSeedPrefix(); });
    checkSeedPrefixTimer_->scheduleTimeout(
        kCheckSeedPrefixInterval, true /*periodic*/);
  } else {
    // Start allocation from user provided seed prefix
    scheduleTimeout(0ms, [this]() noexcept {
      if (isSeedPrefixValid(*seedPrefix_, allocPrefixLen_)) {
        startAlloc();
      }
    });
  }
}

void
PrefixAllocator::checkSeedPrefix() {
  CHECK(!seedPrefix_) << "seedPrefix must not already exist...";

  auto prefixAllocParams = getValueByKey(kPrefixAllocParamKey);
  if (!prefixAllocParams) {
    VLOG(4) << "PrefixAllocator: Did not get value for key from KV Store "
            << kPrefixAllocParamKey;
    return;
  }

  std::string seedPrefixStr;
  folly::split(
      kSeedPrefixAllocLenSeparator,
      *prefixAllocParams,
      seedPrefixStr,
      allocPrefixLen_);

  folly::CIDRNetwork prefix;
  try {
    prefix = folly::IPAddress::createNetwork(
        seedPrefixStr, -1 /* default CIDR */, false /* do not apply mask */);
  } catch (std::exception const& err) {
    LOG(ERROR) << "Invalid seed prefix: " << seedPrefixStr
               << folly::exceptionStr(err);
    return;
  }

  if (!isSeedPrefixValid(prefix, allocPrefixLen_)) {
    LOG(ERROR) << "Invalid seed prefix: "
               << folly::IPAddress::networkToString(prefix);
    return;
  }

  // We got a valid prefix from KvStore. Use it and cancel timer
  checkSeedPrefixTimer_->cancelTimeout();
  seedPrefix_.emplace(prefix);
  startAlloc();
}

void
PrefixAllocator::startAlloc() {
  CHECK(seedPrefix_) << "Seed prefix is not set";
  CHECK(isSeedPrefixValid(*seedPrefix_, allocPrefixLen_))
      << "Seed prefix is not valid";

  VLOG(2) << "Starting prefix allocation with seed prefix: "
          << folly::IPAddress::networkToString(*seedPrefix_)
          << ", allocated prefix length: " << allocPrefixLen_;

  // power 2
  prefixCount_ = 0x1 << (allocPrefixLen_ - seedPrefix_->second);

  initMyPrefix();

  // start range allocation
  rangeAllocator_->startAllocator(
      std::make_pair(0, prefixCount_ - 1), myPrefixIndex_);
}

bool
PrefixAllocator::allPrefixAllocated() {
  CHECK(isInEventLoop());
  return rangeAllocator_->isRangeConsumed();
}

folly::Optional<uint32_t>
PrefixAllocator::getMyPrefixIndexFromKvStore() {
  VLOG(4) << "See if I am already allocated a prefix in kvstore";

  // This is not done at cold start, but rather some time later. So
  // probably we have synchronized with existing kv store if it is present
  return rangeAllocator_->getValueFromKvStore();
}

folly::Optional<uint32_t>
PrefixAllocator::loadPrefixFromDisk() const {
  CHECK(seedPrefix_) << "Seed prefix is not set";
  auto maybeThriftAllocPrefix =
      configStoreClient_.loadThriftObj<thrift::AllocPrefix>(kConfigKey);
  if (maybeThriftAllocPrefix.hasError()) {
    return folly::none;
  }
  auto thriftAllocPrefix = maybeThriftAllocPrefix.value();

  auto addr = toIPAddress(thriftAllocPrefix.seedPrefix.prefixAddress);
  auto prefixLen = thriftAllocPrefix.seedPrefix.prefixLength;
  folly::CIDRNetwork seedPrefix = {addr, prefixLen};
  auto allocPrefixLen = static_cast<uint32_t>(thriftAllocPrefix.allocPrefixLen);
  auto allocPrefixIndex =
      static_cast<uint32_t>(thriftAllocPrefix.allocPrefixIndex);
  VLOG(4) << "Loading prefix " << allocPrefixIndex;
  if (seedPrefix == *seedPrefix_ && allocPrefixLen == allocPrefixLen_) {
    return allocPrefixIndex;
  }

  LOG(ERROR)
      << "Prefix allocation parameters changed, do not use prefix from disk";
  if (*seedPrefix_ != seedPrefix) {
    LOG(ERROR) << "Seed prefix changed, new: "
               << folly::IPAddress::networkToString(*seedPrefix_)
               << "vs old: " << folly::IPAddress::networkToString(seedPrefix);
  }
  if (allocPrefixLen_ != allocPrefixLen) {
    LOG(ERROR) << "Allocated prefix length changed, new: " << allocPrefixLen_
               << "vs old: " << allocPrefixLen;
  }
  return folly::none;
}

void
PrefixAllocator::savePrefixToDisk(folly::Optional<uint32_t> prefixIndex) {
  if (!prefixIndex) {
    VLOG(4) << "Erasing prefix-allocator info from persistent config.";
    configStoreClient_.erase(kConfigKey);
    return;
  }

  VLOG(4) << "Saving prefix-allocator info to persistent config with index "
          << *prefixIndex;

  auto prefix = thrift::IpPrefix(
      apache::thrift::FRAGILE,
      toBinaryAddress(seedPrefix_->first),
      seedPrefix_->second);
  thrift::AllocPrefix thriftAllocPrefix(
      apache::thrift::FRAGILE,
      prefix,
      static_cast<int64_t>(allocPrefixLen_),
      static_cast<int64_t>(*prefixIndex));

  auto res = configStoreClient_.storeThriftObj(kConfigKey, thriftAllocPrefix);
  if (!(res.hasValue() && res.value())) {
    LOG(ERROR) << "Error saving prefix-allocator info to persistent config. "
               << res.error();
  }
}

void
PrefixAllocator::initMyPrefix() {
  VLOG(4) << "Initialize my prefix";

  // initialize my prefix per the following preferrence:
  // from file > from kvstore > generate new
  myPrefixIndex_ = loadPrefixFromDisk();

  if (myPrefixIndex_) {
    return;
  }
  VLOG(4) << "Initial prefix not loaded from file, try kv store next";
  const auto myPrefixIndex = getMyPrefixIndexFromKvStore();
  if (myPrefixIndex) {
    myPrefixIndex_ = myPrefixIndex;
    LOG(INFO) << "Got initial prefix from kvstore: " << *myPrefixIndex_;
  } else {
    myPrefixIndex_ = hasher(myNodeName_) % prefixCount_;
    VLOG(4) << "Generate new initial prefix: " << *myPrefixIndex_;
  }
}

void
PrefixAllocator::applyMyPrefix(folly::Optional<uint32_t> prefixIndex) {
  CHECK(seedPrefix_) << "Seed prefix is not set";

  // Save information to disk
  savePrefixToDisk(prefixIndex);

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
    prefix = getNthPrefix(*seedPrefix_, allocPrefixLen_, *prefixIndex, true);
  }

  // Flush existing loopback addresses
  if (setLoopbackAddress_) {
    LOG(INFO) << "Flushing existing addresses from interface "
              << loopbackIfaceName_;
    if (!flushIfaceAddrs(
            loopbackIfaceName_, *seedPrefix_, overrideGlobalAddress_)) {
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
    folly::Optional<uint32_t> newPrefix) {
  fbzmq::LogSample sample{};

  sample.addString("event", event);
  sample.addString("entity", "PrefixAllocator");
  sample.addString("node_name", myNodeName_);
  if (oldPrefix) {
    sample.addString("old_prefix",
      folly::IPAddress::networkToString(
        getNthPrefix(*seedPrefix_, allocPrefixLen_, *oldPrefix, true)));
  }

  if (newPrefix) {
    sample.addString("new_prefix",
      folly::IPAddress::networkToString(
        getNthPrefix(*seedPrefix_, allocPrefixLen_, *newPrefix, true)));
  }

  zmqMonitorClient_.addEventLog(fbzmq::thrift::EventLog(
      apache::thrift::FRAGILE,
      Constants::kEventLogCategory,
      {sample.toJson()}));
}

folly::Optional<std::string>
PrefixAllocator::getValueByKey(const std::string& keyName) noexcept {
  const auto maybeVal = kvStoreClient_->getKey(keyName);
  if (maybeVal) {
    return maybeVal->value;
  }
  return folly::none;
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

} // namespace openr
