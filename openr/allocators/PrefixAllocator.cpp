/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Format.h>
#include <folly/futures/Promise.h>
#include <folly/logging/xlog.h>

#include <openr/allocators/PrefixAllocator.h>
#include <openr/common/Constants.h>
#include <openr/common/Util.h>
#include <openr/nl/NetlinkTypes.h>

namespace {

// key for the persist config on disk
const std::string kConfigKey{"prefix-allocator-config"};

} // namespace

namespace openr {

PrefixAllocator::PrefixAllocator(
    AreaId const& area,
    std::shared_ptr<const Config> config,
    fbnl::NetlinkProtocolSocket* nlSock,
    KvStore* kvStore,
    PersistentStore* configStore,
    messaging::ReplicateQueue<PrefixEvent>& prefixUpdatesQueue,
    messaging::ReplicateQueue<LogSample>& logSampleQueue,
    messaging::ReplicateQueue<KeyValueRequest>& kvRequestQueue,
    std::chrono::milliseconds syncInterval)
    : myNodeName_(config->getNodeName()),
      syncInterval_(syncInterval),
      enableKvRequestQueue_(
          config->getConfig().get_enable_kvstore_request_queue()),
      setLoopbackAddress_(
          *config->getPrefixAllocationConfig().set_loopback_addr_ref()),
      overrideGlobalAddress_(
          *config->getPrefixAllocationConfig().override_loopback_addr_ref()),
      loopbackIfaceName_(
          *config->getPrefixAllocationConfig().loopback_interface_ref()),
      prefixForwardingType_(*config->getConfig().prefix_forwarding_type_ref()),
      prefixForwardingAlgorithm_(
          *config->getConfig().prefix_forwarding_algorithm_ref()),
      area_(area),
      nlSock_(nlSock),
      kvStore_(kvStore),
      configStore_(configStore),
      prefixUpdatesQueue_(prefixUpdatesQueue),
      logSampleQueue_(logSampleQueue),
      kvRequestQueue_(kvRequestQueue) {
  // check non-empty module ptr
  CHECK(nlSock_);
  CHECK(configStore_);
  CHECK(kvStore);

  // Create KvStore client
  kvStoreClient_ =
      std::make_unique<KvStoreClientInternal>(this, myNodeName_, kvStore);

  // Let the magic begin. Start allocation as per allocMode
  switch (*config->getPrefixAllocationConfig().prefix_allocation_mode_ref()) {
  case thrift::PrefixAllocationMode::DYNAMIC_LEAF_NODE:
    XLOG(INFO) << "DYNAMIC_LEAF_NODE";
    dynamicAllocationLeafNode();
    break;
  case thrift::PrefixAllocationMode::DYNAMIC_ROOT_NODE:
    XLOG(INFO) << "DYNAMIC_ROOT_NODE";
    dynamicAllocationRootNode(config->getPrefixAllocationParams());
    break;
  case thrift::PrefixAllocationMode::STATIC:
    XLOG(INFO) << "STATIC";
    staticAllocation();
    break;
  }

  // create retryTimer for applying prefix
  retryTimer_ = folly::AsyncTimeout::make(
      *getEvb(), [this]() noexcept { applyMyPrefix(); });
}

void
PrefixAllocator::stop() {
  // Stop KvStoreClient first
  kvStoreClient_->stop();
  XLOG(INFO) << "KvStoreClient successfully stopped.";

  // Invoke stop method of super class
  OpenrEventBase::stop();
}

void
PrefixAllocator::staticAllocation() {
  // subscribe for incremental updates of static prefix allocation key
  kvStoreClient_->subscribeKey(
      area_,
      Constants::kStaticPrefixAllocParamKey.toString(),
      [&](std::string const& key, std::optional<thrift::Value> value) {
        CHECK_EQ(Constants::kStaticPrefixAllocParamKey.toString(), key);
        if (value.has_value()) {
          processStaticPrefixAllocUpdate(value.value());
        }
      },
      false);

  // get initial value if missed out in incremental updates (one time only)
  initTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    // If we already have received initial value from KvStore then just skip
    // this step
    if (allocParams_.has_value()) {
      return;
    }

    // 1) Get initial value from KvStore!
    try {
      thrift::KeyGetParams getStaticAllocParams;
      getStaticAllocParams.keys_ref()->emplace_back(
          Constants::kStaticPrefixAllocParamKey.toString());
      auto maybeGetKey =
          kvStore_->semifuture_getKvStoreKeyVals(area_, getStaticAllocParams)
              .getTry(Constants::kReadTimeout);
      if (not maybeGetKey.hasValue()) {
        XLOG(ERR) << fmt::format(
            "Failed to retrieve prefix allocation key: {}. Exception: {}",
            Constants::kStaticPrefixAllocParamKey.toString(),
            folly::exceptionStr(maybeGetKey.exception()));
      } else {
        auto pub = *maybeGetKey.value();
        auto it = pub.keyVals_ref()->find(
            Constants::kStaticPrefixAllocParamKey.toString());
        if (it == pub.keyVals_ref()->end()) {
          XLOG(ERR) << "Prefix allocation key: "
                    << Constants::kStaticPrefixAllocParamKey.toString()
                    << " not found in KvStore, area: " << area_.t;
        } else {
          // Successful key-value retrieval. Process static allocation update.
          processStaticPrefixAllocUpdate(it->second);
          return;
        }
      }
    } catch (const folly::FutureTimeout&) {
      XLOG(ERR) << "Timed out retrieving prefix allocation key: "
                << Constants::kStaticPrefixAllocParamKey.toString();
    }

    // 2) Start prefix allocator from previously configured params. Resume
    // from where we left earlier!
    auto maybeThriftAllocPrefix =
        configStore_->loadThriftObj<thrift::AllocPrefix>(kConfigKey).get();
    if (maybeThriftAllocPrefix.hasValue()) {
      const auto oldAllocParams = std::make_pair(
          toIPNetwork(*maybeThriftAllocPrefix->seedPrefix_ref()),
          static_cast<uint8_t>(*maybeThriftAllocPrefix->allocPrefixLen_ref()));
      allocParams_ = oldAllocParams;
      applyMyPrefixIndex(*maybeThriftAllocPrefix->allocPrefixIndex_ref());
      return;
    }

    // If we weren't able to get alloc parameters so far, either from disk
    // or kvstore then let's bail out, flush out previously elected address
    // (from PrefixManager and loopback iface).
    if (!allocParams_.has_value()) {
      XLOG(WARNING)
          << "Clearing previous prefix allocation state on failure to load "
          << "allocation parameters from disk as well as KvStore.";
      applyState_ = std::make_pair(true, std::nullopt);
      applyMyPrefix();
      return;
    }
  });
  initTimer_->scheduleTimeout(0ms);
}

void
PrefixAllocator::dynamicAllocationLeafNode() {
  // subscribe for incremental updates of seed prefix
  kvStoreClient_->subscribeKey(
      area_,
      Constants::kSeedPrefixAllocParamKey.toString(),
      [&](std::string const& key, std::optional<thrift::Value> value) {
        CHECK_EQ(Constants::kSeedPrefixAllocParamKey.toString(), key);
        if (value.has_value()) {
          processAllocParamUpdate(value.value());
        }
      },
      false);

  kvStoreClient_->subscribeKey(
      area_,
      Constants::kStaticPrefixAllocParamKey.toString(),
      [&](std::string const& key, std::optional<thrift::Value> value) {
        CHECK_EQ(Constants::kStaticPrefixAllocParamKey.toString(), key);
        if (value.has_value()) {
          processNetworkAllocationsUpdate(value.value());
        }
      },
      false);

  // get initial value if missed out in incremental updates (one time only)
  initTimer_ = folly::AsyncTimeout::make(*getEvb(), [this]() noexcept {
    // If we already have received initial value from KvStore then just skip
    // this step
    if (allocParams_.has_value()) {
      return;
    }

    // 1) Get initial value from KvStore!
    try {
      thrift::KeyGetParams params;
      params.keys_ref()->emplace_back(
          Constants::kSeedPrefixAllocParamKey.toString());
      auto maybeGetKey = kvStore_->semifuture_getKvStoreKeyVals(area_, params)
                             .getTry(Constants::kReadTimeout);
      if (not maybeGetKey.hasValue()) {
        XLOG(ERR) << fmt::format(
            "Failed to retrieve seed prefix allocation key: {}. Exception: {}",
            Constants::kSeedPrefixAllocParamKey.toString(),
            folly::exceptionStr(maybeGetKey.exception()));
      } else {
        auto pub = *maybeGetKey.value();
        auto it = pub.keyVals_ref()->find(
            Constants::kSeedPrefixAllocParamKey.toString());
        if (it == pub.keyVals_ref()->end()) {
          XLOG(ERR) << "Seed prefix alloc key: "
                    << Constants::kSeedPrefixAllocParamKey.toString()
                    << " not found in KvStore, area: " << area_.t;
        } else {
          // Successful key-value retrieval. Process allocation param update.
          processAllocParamUpdate(it->second);
          return;
        }
      }
    } catch (const folly::FutureTimeout&) {
      XLOG(ERR) << "Timed out retrieving seed prefix allocation key: "
                << Constants::kSeedPrefixAllocParamKey.toString();
      ;
    }

    // 2) Start prefix allocator from previously configured params. Resume
    // from where we left earlier!
    auto maybeThriftAllocPrefix =
        configStore_->loadThriftObj<thrift::AllocPrefix>(kConfigKey).get();
    if (maybeThriftAllocPrefix.hasValue()) {
      const auto oldAllocParams = std::make_pair(
          toIPNetwork(*maybeThriftAllocPrefix->seedPrefix_ref()),
          static_cast<uint8_t>(*maybeThriftAllocPrefix->allocPrefixLen_ref()));
      startAllocation(oldAllocParams);
      return;
    }

    // If we weren't able to get alloc parameters so far, either from disk
    // or kvstore then let's bail out and stop allocation process and
    // withdraw our prefixes from PrefixManager as well as from loopback
    // interface.
    if (!allocParams_.has_value()) {
      XLOG(WARNING)
          << "Clearing previous prefix allocation state on failure to load "
          << "allocation parameters from disk as well as KvStore.";
      applyState_ = std::make_pair(true, std::nullopt);
      applyMyPrefix();
      return;
    }
  });
  initTimer_->scheduleTimeout(0ms);
}

void
PrefixAllocator::dynamicAllocationRootNode(
    PrefixAllocationParams const& allocParams) {
  // Some sanity checks
  const auto& seedPrefix = allocParams.first;
  const auto& allocPrefixLen = allocParams.second;
  CHECK_GT(allocPrefixLen, seedPrefix.second)
      << "Allocation prefix length must be greater than seed prefix length.";
  // Start allocation from user provided seed prefix
  initTimer_ =
      folly::AsyncTimeout::make(*getEvb(), [this, allocParams]() noexcept {
        startAllocation(allocParams);
      });
  initTimer_->scheduleTimeout(0ms);
}

std::optional<uint32_t>
PrefixAllocator::getMyPrefixIndex() {
  if (getEvb()->isInEventBaseThread()) {
    return myPrefixIndex_;
  }

  // Otherwise enqueue request in eventloop and wait for result to be populated
  folly::Promise<std::optional<uint32_t>> promise;
  auto future = promise.getFuture();
  runInEventBaseThread([this, promise = std::move(promise)]() mutable {
    promise.setValue(myPrefixIndex_);
  });
  return std::move(future).get();
}

PrefixAllocationParams
PrefixAllocator::parseParamsStr(const std::string& paramStr) {
  // Parse string to get seed-prefix and alloc-prefix-length
  std::string seedPrefixStr;
  uint8_t allocPrefixLen;
  folly::split(
      Constants::kSeedPrefixAllocLenSeparator.toString(),
      paramStr,
      seedPrefixStr,
      allocPrefixLen);
  return Config::createPrefixAllocationParams(seedPrefixStr, allocPrefixLen);
}

void
PrefixAllocator::processStaticPrefixAllocUpdate(thrift::Value const& value) {
  CHECK(value.value_ref().has_value());

  // Parse thrift::Value into thrift::StaticAllocation
  thrift::StaticAllocation staticAlloc;
  try {
    staticAlloc = readThriftObjStr<thrift::StaticAllocation>(
        *value.value_ref(), serializer_);
  } catch (std::exception const& e) {
    XLOG(ERR) << "Error parsing static prefix allocation value. Error: "
              << folly::exceptionStr(e);
    if (allocParams_) {
      applyMyPrefixIndex(std::nullopt);
      allocParams_ = std::nullopt;
    }
    return;
  }
  XLOG(DBG1) << "Processing static prefix allocation update";

  // Look for my prefix in static allocation map
  auto myPrefixIt = staticAlloc.nodePrefixes_ref()->find(myNodeName_);

  // Withdraw my prefix if not found
  if (myPrefixIt == staticAlloc.nodePrefixes_ref()->end() and allocParams_) {
    XLOG(DBG2) << "Lost prefix";
    applyState_ = std::make_pair(true, std::nullopt);
    applyMyPrefixIndex(std::nullopt);
    allocParams_ = std::nullopt;
  }

  // Advertise my prefix if found
  if (myPrefixIt != staticAlloc.nodePrefixes_ref()->end()) {
    XLOG(DBG2) << "Received new prefix";
    const auto prefix = toIPNetwork(myPrefixIt->second);
    const auto newParams = std::make_pair(prefix, prefix.second);
    if (allocParams_ == newParams) {
      XLOG(INFO) << "New and old params are same. Skipping";
    } else if (allocParams_.has_value()) {
      // withdraw old prefix
      applyMyPrefixIndex(std::nullopt);
    }
    // create alloc params so that we share same workflow as of SEEDED mode
    // seedPrefix length is same as alloc prefix length and my prefix index is 0
    allocParams_ = std::make_pair(prefix, prefix.second); // seed prefix
    applyMyPrefixIndex(0); // 0th index is what we own.
  }
}

void
PrefixAllocator::processAllocParamUpdate(thrift::Value const& value) {
  CHECK(value.value_ref().has_value());

  std::optional<PrefixAllocationParams> params = std::nullopt;
  auto paramStr = value.value_ref().value();
  try {
    params = parseParamsStr(paramStr);
  } catch (std::exception const& e) {
    XLOG(ERR) << fmt::format(
        "Malformed prefix-allocator params [{}]: {}",
        paramStr,
        folly::exceptionStr(e));
  }

  startAllocation(params);
}

bool
PrefixAllocator::checkE2eAllocIndex(uint32_t index) {
  return (e2eAllocIndex_.second.find(index) != e2eAllocIndex_.second.end());
}

void
PrefixAllocator::processNetworkAllocationsUpdate(
    thrift::Value const& e2eValue) {
  CHECK(e2eValue.value_ref().has_value());
  if (!allocParams_.has_value()) {
    return;
  }
  uint32_t allocPrefixLen = allocParams_->second;
  uint32_t prefixLen = allocParams_->first.second;

  thrift::StaticAllocation staticAlloc;
  try {
    staticAlloc = readThriftObjStr<thrift::StaticAllocation>(
        *e2eValue.value_ref(), serializer_);
    /* skip if same version */
    if (e2eAllocIndex_.first == *e2eValue.version_ref()) {
      return;
    }
    XLOG(INFO) << fmt::format(
        "Updating prefix index from {}",
        Constants::kStaticPrefixAllocParamKey.toString());
    e2eAllocIndex_.second.clear();
    e2eAllocIndex_.first = *e2eValue.version_ref();
    // extract prefix index from e2e network allocations
    for (auto const& ip : *staticAlloc.nodePrefixes_ref()) {
      const auto pfix = toIPNetwork(ip.second);
      auto index = bitStrValue(pfix.first, prefixLen, allocPrefixLen - 1);
      e2eAllocIndex_.second.emplace(index);
    }
    // collision after the update, restart allocation process
    if (myPrefixIndex_.has_value() &&
        e2eAllocIndex_.second.find(myPrefixIndex_.value()) !=
            e2eAllocIndex_.second.end()) {
      XLOG(INFO) << fmt::format(
          "Index {} exits in {}, restarting prefix allocator",
          myPrefixIndex_.value(),
          Constants::kStaticPrefixAllocParamKey.toString());
      startAllocation(allocParams_, false);
    }
  } catch (std::exception const& e) {
    XLOG(ERR) << "Error parsing static prefix allocation value. Error: "
              << folly::exceptionStr(e);
    return;
  }
}

uint32_t
PrefixAllocator::getPrefixCount(
    PrefixAllocationParams const& allocParams) noexcept {
  auto const& seedPrefix = allocParams.first;
  auto const& allocPrefixLen = allocParams.second;

  // If range of prefix alloc is greater than 32 bit integer we won't let it
  // overflow.
  return (1 << std::min(31, allocPrefixLen - seedPrefix.second));
}

std::optional<uint32_t>
PrefixAllocator::loadPrefixIndexFromKvStore() {
  XLOG(DBG4) << "See if I am already allocated a prefix in kvstore";

  // This is not done at cold start, but rather some time later. So
  // probably we have synchronized with existing kv store if it is present
  return rangeAllocator_->getValueFromKvStore();
}

std::optional<uint32_t>
PrefixAllocator::loadPrefixIndexFromDisk() {
  auto maybeThriftAllocPrefix =
      configStore_->loadThriftObj<thrift::AllocPrefix>(kConfigKey).get();
  if (maybeThriftAllocPrefix.hasError()) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*maybeThriftAllocPrefix->allocPrefixIndex_ref());
}

void
PrefixAllocator::savePrefixIndexToDisk(std::optional<uint32_t> prefixIndex) {
  CHECK(allocParams_.has_value());

  if (!prefixIndex) {
    XLOG(DBG4) << "Erasing prefix-allocator info from persistent config.";
    configStore_->erase(kConfigKey).get();
    return;
  }

  XLOG(DBG4) << "Saving prefix-allocator info to persistent config with index "
             << *prefixIndex;
  auto prefix = createIpPrefix(
      toBinaryAddress(allocParams_->first.first), allocParams_->first.second);
  auto thriftAllocPrefix = createAllocPrefix(
      prefix,
      static_cast<int64_t>(allocParams_->second),
      static_cast<int64_t>(*prefixIndex));
  configStore_->storeThriftObj(kConfigKey, thriftAllocPrefix).get();
}

uint32_t
PrefixAllocator::getInitPrefixIndex() {
  // initialize my prefix per the following preferrence:
  // from file > from kvstore > generate new

  // Try to get prefix index from disk
  const auto diskPrefixIndex = loadPrefixIndexFromDisk();
  if (diskPrefixIndex.has_value()) {
    XLOG(INFO) << "Got initial prefix index from disk: " << *diskPrefixIndex;
    return diskPrefixIndex.value();
  }

  // Try to get prefix index from KvStore
  const auto kvstorePrefixIndex = loadPrefixIndexFromKvStore();
  if (kvstorePrefixIndex.has_value()) {
    XLOG(INFO) << "Got initial prefix index from KvStore: "
               << *kvstorePrefixIndex;
    return kvstorePrefixIndex.value();
  }

  // Generate a new random prefix index
  if (allocParams_.has_value()) {
    uint32_t hashPrefixIndex =
        hasher(myNodeName_) % getPrefixCount(*allocParams_);
    XLOG(INFO) << "Generate new initial prefix index: " << hashPrefixIndex;
    return hashPrefixIndex;
  }

  // `0` is always a valid index in valid range
  return 0;
}

void
PrefixAllocator::startAllocation(
    std::optional<PrefixAllocationParams> const& allocParams,
    bool checkParams) {
  // Some informative logging
  if (allocParams_.has_value() and allocParams.has_value()) {
    if (checkParams and allocParams_ == allocParams) {
      XLOG(INFO) << "New and old params are same. Skipping";
      return;
    }
    XLOG(WARNING)
        << " Prefix allocation parameters are changing,"
        << " or duplicate address detected. \n"
        << "  Old: " << folly::IPAddress::networkToString(allocParams_->first)
        << ", " << static_cast<int16_t>(allocParams_->second) << "\n"
        << "  New: " << folly::IPAddress::networkToString(allocParams->first)
        << ", " << static_cast<int16_t>(allocParams->second);
  }
  if (allocParams_.has_value() and not allocParams.has_value()) {
    XLOG(WARNING)
        << "Prefix allocation parameters are not valid anymore. \n"
        << "  Old: " << folly::IPAddress::networkToString(allocParams_->first)
        << ", " << static_cast<int16_t>(allocParams_->second) << "\n";
  }
  if (not allocParams_.has_value() and allocParams.has_value()) {
    XLOG(INFO)
        << "Prefix allocation parameters have been received. \n"
        << "  New: " << folly::IPAddress::networkToString(allocParams->first)
        << ", " << static_cast<int16_t>(allocParams->second);
  }
  logPrefixEvent(
      "ALLOC_PARAMS_UPDATE",
      std::nullopt,
      std::nullopt,
      allocParams_ /* old params */,
      allocParams /* new params */);

  // Update local state
  rangeAllocator_.reset();
  if (allocParams_) {
    applyMyPrefixIndex(std::nullopt); // Clear local state
  }
  CHECK(!myPrefixIndex_.has_value());
  allocParams_ = allocParams;

  if (!allocParams_.has_value()) {
    return;
  }

  // create range allocator to get unique prefixes
  rangeAllocator_ = std::make_unique<RangeAllocator<uint32_t>>(
      area_,
      myNodeName_,
      Constants::kPrefixAllocMarker.toString(),
      kvStore_,
      kvStoreClient_.get(),
      [this](std::optional<uint32_t> newPrefixIndex) noexcept {
        applyMyPrefixIndex(newPrefixIndex);
      },
      kvRequestQueue_,
      enableKvRequestQueue_,
      syncInterval_,
      // no need for randomness since "collision" is harmless
      syncInterval_ + 1ms,
      // do not allow override
      false,
      [this](uint32_t allocIndex) noexcept -> bool {
        return checkE2eAllocIndex(allocIndex);
      },
      Constants::kRangeAllocTtl);

  // start range allocation
  XLOG(INFO) << "Starting prefix allocation with seed prefix: "
             << folly::IPAddress::networkToString(allocParams_->first)
             << ", allocation prefix length: "
             << static_cast<int16_t>(allocParams_->second);
  const uint32_t prefixCount = getPrefixCount(*allocParams_);
  uint32_t startIndex = 0;
  uint32_t endIndex = prefixCount - 1;

  // For IPv4, if the prefix length is 32
  // then the first and last IP in that subnet are not valid host addresses.
  if (allocParams_->first.first.isV4() && allocParams_->second == 32) {
    startIndex += 1;
    endIndex -= 1;
  }

  rangeAllocator_->startAllocator(
      std::make_pair(startIndex, endIndex), getInitPrefixIndex());
}

void
PrefixAllocator::applyMyPrefixIndex(std::optional<uint32_t> prefixIndex) {
  // Silently return if nothing changed. For e.g.
  if (myPrefixIndex_ == prefixIndex) {
    return;
  }

  CHECK(allocParams_.has_value()) << "Alloc parameters are not set.";

  // Save information to disk
  savePrefixIndexToDisk(prefixIndex);

  if (prefixIndex and !myPrefixIndex_) {
    XLOG(INFO) << "Elected new prefixIndex " << *prefixIndex;
    logPrefixEvent("PREFIX_ELECTED", std::nullopt, prefixIndex);
  } else if (prefixIndex and myPrefixIndex_) {
    XLOG(INFO) << "Updating prefixIndex to " << *prefixIndex << " from "
               << *myPrefixIndex_;
    logPrefixEvent("PREFIX_UPDATED", myPrefixIndex_, prefixIndex);
  } else if (myPrefixIndex_) {
    XLOG(INFO) << "Lost previously allocated prefixIndex " << *myPrefixIndex_;
    logPrefixEvent("PREFIX_LOST", myPrefixIndex_, std::nullopt);
  }

  // Set local state
  myPrefixIndex_ = prefixIndex;

  // Create network prefix to announce and loopback address to assign
  std::optional<folly::CIDRNetwork> prefix = std::nullopt;
  if (prefixIndex) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    prefix = getNthPrefix(seedPrefix, allocPrefixLen, *prefixIndex);
  }

  // Announce my prefix
  applyState_ = std::make_pair(true, std::move(prefix));
  applyMyPrefix();
}

void
PrefixAllocator::applyMyPrefix() {
  if (!applyState_.first) {
    return;
  }
  try {
    if (applyState_.second) {
      updateMyPrefix(*applyState_.second);
    } else {
      withdrawMyPrefix();
    }
    applyState_.first = false;
  } catch (const std::exception& ex) {
    XLOG(ERR)
        << "Apply prefix failed, will retry in "
        << Constants::kPrefixAllocatorRetryInterval.count() << " ms address: "
        << (applyState_.second.has_value()
                ? folly::IPAddress::networkToString(applyState_.second.value())
                : "none");
    retryTimer_->scheduleTimeout(Constants::kPrefixAllocatorRetryInterval);
  }
}

void
PrefixAllocator::updateMyPrefix(folly::CIDRNetwork prefix) {
  CHECK(allocParams_.has_value()) << "Alloc parameters are not set.";
  // replace previously allocated prefix with newly allocated one in
  // PrefixManager

  auto prefixEntry = openr::thrift::PrefixEntry();
  *prefixEntry.prefix_ref() = toIpPrefix(prefix);
  prefixEntry.type_ref() = openr::thrift::PrefixType::PREFIX_ALLOCATOR;
  prefixEntry.forwardingType_ref() = prefixForwardingType_;
  prefixEntry.forwardingAlgorithm_ref() = prefixForwardingAlgorithm_;
  prefixEntry.tags_ref()->emplace("AUTO-ALLOCATED");
  // Metrics
  {
    auto& metrics = prefixEntry.metrics_ref().value();
    metrics.path_preference_ref() = Constants::kDefaultPathPreference;
    metrics.source_preference_ref() = Constants::kDefaultSourcePreference;
  }

  PrefixEvent event(
      PrefixEventType::SYNC_PREFIXES_BY_TYPE,
      thrift::PrefixType::PREFIX_ALLOCATOR,
      {prefixEntry});
  prefixUpdatesQueue_.push(std::move(event));

  // existing global prefixes
  std::vector<folly::CIDRNetwork> oldPrefixes{};
  try {
    oldPrefixes =
        semifuture_getIfAddrs(
            loopbackIfaceName_, prefix.first.family(), RT_SCOPE_UNIVERSE)
            .get();
  } catch (const fbnl::NlException& ex) {
    XLOG(ERR)
        << "Failed to get iface addresses from NetlinkProtocolSocket. Error: "
        << folly::exceptionStr(ex);
    return;
  }

  // desired global prefixes
  auto loopbackPrefix = createLoopbackPrefix(prefix);
  std::vector<folly::CIDRNetwork> toSyncPrefixes{loopbackPrefix};

  // get a list of prefixes need to be deleted
  std::vector<folly::CIDRNetwork> toDeletePrefixes;
  std::set_difference(
      oldPrefixes.begin(),
      oldPrefixes.end(),
      toSyncPrefixes.begin(),
      toSyncPrefixes.end(),
      std::inserter(toDeletePrefixes, toDeletePrefixes.begin()));

  if (toDeletePrefixes.empty() && !oldPrefixes.empty()) {
    XLOG(INFO) << "Prefix not changed";
    return;
  }

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
      toSyncPrefixes.emplace_back(toDeletePrefix);
      continue;
    }

    XLOG(INFO) << "Will delete address "
               << folly::IPAddress::networkToString(toDeletePrefix)
               << " on interface " << loopbackIfaceName_;
  }

  // Assign new address to loopback
  if (setLoopbackAddress_) {
    XLOG(INFO) << "Assigning address: "
               << folly::IPAddress::networkToString(loopbackPrefix)
               << " on interface " << loopbackIfaceName_;
    toSyncPrefixes.emplace_back(loopbackPrefix);

    try {
      semifuture_syncIfAddrs(
          loopbackIfaceName_,
          prefix.first.family(),
          RT_SCOPE_UNIVERSE,
          toSyncPrefixes)
          .get();
    } catch (const std::exception& ex) {
      XLOG(ERR) << "Failed to sync iface addresses for interface: "
                << loopbackIfaceName_ << " from NetlinkProtocolSocket. Error: "
                << folly::exceptionStr(ex);
      throw;
    }
  }
}

void
PrefixAllocator::withdrawMyPrefix() {
  // Flush existing loopback addresses
  if (setLoopbackAddress_ and allocParams_.has_value()) {
    XLOG(INFO) << "Flushing existing addresses from interface "
               << loopbackIfaceName_;

    const auto& prefix = allocParams_->first;
    if (overrideGlobalAddress_) {
      // provide empty addresses for address withdrawn
      std::vector<folly::CIDRNetwork> networks{};
      try {
        semifuture_syncIfAddrs(
            loopbackIfaceName_,
            prefix.first.family(),
            RT_SCOPE_UNIVERSE,
            networks)
            .get();
      } catch (const std::exception& ex) {
        XLOG(ERR) << "Failed to sync iface addresses for interface: "
                  << loopbackIfaceName_
                  << " from NetlinkProtocolSocket. Error: "
                  << folly::exceptionStr(ex);
        throw;
      }
    } else {
      try {
        // delele interface address
        semifuture_addRemoveIfAddr(false, loopbackIfaceName_, {prefix}).get();
      } catch (const fbnl::NlException& ex) {
        XLOG(ERR) << "Failed to del iface address: "
                  << folly::IPAddress::networkToString(prefix)
                  << " from NetlinkProtocolSocket. Error: "
                  << folly::exceptionStr(ex);
        throw;
      }
    }
  }

  // withdraw prefix via replicate queue
  PrefixEvent event(
      PrefixEventType::WITHDRAW_PREFIXES_BY_TYPE,
      thrift::PrefixType::PREFIX_ALLOCATOR);
  prefixUpdatesQueue_.push(std::move(event));
}

folly::SemiFuture<folly::Unit>
PrefixAllocator::semifuture_syncIfAddrs(
    std::string iface,
    int16_t family,
    int16_t scope,
    std::vector<folly::CIDRNetwork> newAddrs) {
  std::vector<folly::SemiFuture<int>> futures;
  const auto ifName = iface; // Copy intended
  const auto ifIndex = getIfIndex(ifName).value();

  auto networks = folly::gen::from(newAddrs) |
      folly::gen::mapped([](const folly::CIDRNetwork& network) {
                    return folly::IPAddress::networkToString(network);
                  }) |
      folly::gen::as<std::vector<std::string>>();

  XLOG(INFO) << "Syncing addresses on interface " << iface
             << ", family=" << family << ", scope=" << scope
             << ", addresses=" << folly::join(",", networks);

  // fetch existing iface address as std::vector<folly::CIDRNetwork>
  auto oldAddrs = semifuture_getIfAddrs(iface, family, scope).get();

  // Add new addresses
  for (auto& newAddr : newAddrs) {
    // Skip adding existing addresse
    if (std::find(oldAddrs.cbegin(), oldAddrs.cend(), newAddr) !=
        oldAddrs.cend()) {
      continue;
    }
    // Add non-existing new address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(newAddr);
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->addIfAddress(builder.build()));
  }

  // Delete old addresses
  for (auto& oldAddr : oldAddrs) {
    // Skip removing new addresse
    if (std::find(newAddrs.cbegin(), newAddrs.cend(), oldAddr) !=
        newAddrs.cend()) {
      continue;
    }
    // Remove non-existing old address
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(oldAddr);
    builder.setIfIndex(ifIndex);
    builder.setScope(scope);
    futures.emplace_back(nlSock_->deleteIfAddress(builder.build()));
  }

  // Collect all futures
  return collectAll(std::move(futures))
      .deferValue([](std::vector<folly::Try<int>>&& retvals) {
        for (auto& retval : retvals) {
          const int ret = std::abs(retval.value());
          if (ret != 0 && ret != EEXIST && ret != EADDRNOTAVAIL) {
            throw fbnl::NlException("Address add/remove failed.", ret);
          }
        }
        return folly::Unit();
      });
}

folly::SemiFuture<std::vector<folly::CIDRNetwork>>
PrefixAllocator::semifuture_getIfAddrs(
    std::string ifName, int16_t family, int16_t scope) {
  XLOG(INFO) << "Querying addresses for interface " << ifName
             << ", family=" << family << ", scope=" << scope;

  // Get iface index
  const int ifIndex = getIfIndex(ifName).value();

  return nlSock_->getAllIfAddresses().deferValue(
      [ifIndex, family, scope](
          folly::Expected<std::vector<fbnl::IfAddress>, int>&& nlAddrs) {
        if (nlAddrs.hasError()) {
          throw fbnl::NlException("Failed fetching addrs", nlAddrs.error());
        }

        std::vector<folly::CIDRNetwork> addrs{};
        for (auto& nlAddr : nlAddrs.value()) {
          if (nlAddr.getIfIndex() != ifIndex) {
            continue;
          }
          // Apply filter on family if specified
          if (family && nlAddr.getFamily() != family) {
            continue;
          }
          // Apply filter on scope. Must always be specified
          if (nlAddr.getScope() != scope) {
            continue;
          }
          addrs.emplace_back(nlAddr.getPrefix().value());
        }
        return addrs;
      });
}
folly::SemiFuture<folly::Unit>
PrefixAllocator::semifuture_addRemoveIfAddr(
    const bool isAdd,
    const std::string& ifName,
    const std::vector<folly::CIDRNetwork>& networks) {
  auto addrs = folly::gen::from(networks) |
      folly::gen::mapped([](const folly::CIDRNetwork& addr) {
                 return folly::IPAddress::networkToString(addr);
               }) |
      folly::gen::as<std::vector<std::string>>();

  XLOG(INFO) << (isAdd ? "Adding" : "Removing") << " addresses on interface "
             << ifName << ", addresses=" << folly::join(",", addrs);

  // Get iface index
  const int ifIndex = getIfIndex(ifName).value();

  // Add netlink requests
  std::vector<folly::SemiFuture<int>> futures;
  for (const auto& network : networks) {
    fbnl::IfAddressBuilder builder;
    builder.setPrefix(network);
    builder.setIfIndex(ifIndex);
    if (network.first.isLoopback()) {
      builder.setScope(RT_SCOPE_HOST);
    } else if (network.first.isLinkLocal()) {
      builder.setScope(RT_SCOPE_LINK);
    } else {
      builder.setScope(RT_SCOPE_UNIVERSE);
    }
    if (isAdd) {
      futures.emplace_back(nlSock_->addIfAddress(builder.build()));
    } else {
      futures.emplace_back(nlSock_->deleteIfAddress(builder.build()));
    }
  }

  // Accumulate futures into a single one
  return collectAll(std::move(futures))
      .deferValue([](std::vector<folly::Try<int>>&& retvals) {
        for (auto& retval : retvals) {
          const int ret = std::abs(retval.value());
          if (ret != 0 && ret != EEXIST && ret != EADDRNOTAVAIL) {
            throw fbnl::NlException("Address add/remove failed.", ret);
          }
        }
        return folly::Unit();
      });
}

std::optional<int>
PrefixAllocator::getIfIndex(const std::string& ifName) {
  auto links = nlSock_->getAllLinks().get().value();
  for (auto& link : links) {
    if (link.getLinkName() == ifName) {
      return link.getIfIndex();
    }
  }
  return std::nullopt;
}

void
PrefixAllocator::logPrefixEvent(
    std::string event,
    std::optional<uint32_t> oldPrefix,
    std::optional<uint32_t> newPrefix,
    std::optional<PrefixAllocationParams> const& oldAllocParams,
    std::optional<PrefixAllocationParams> const& newAllocParams) {
  LogSample sample{};

  sample.addString("event", event);
  sample.addString("node_name", myNodeName_);
  if (allocParams_.has_value() && oldPrefix) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    sample.addString(
        "old_prefix",
        folly::IPAddress::networkToString(
            getNthPrefix(seedPrefix, allocPrefixLen, *oldPrefix)));
  }

  if (allocParams_.has_value() && newPrefix) {
    auto const& seedPrefix = allocParams_->first;
    auto const& allocPrefixLen = allocParams_->second;
    sample.addString(
        "new_prefix",
        folly::IPAddress::networkToString(
            getNthPrefix(seedPrefix, allocPrefixLen, *newPrefix)));
  }

  if (oldAllocParams.has_value()) {
    sample.addString(
        "old_seed_prefix",
        folly::IPAddress::networkToString(oldAllocParams->first));
    sample.addInt("old_alloc_len", oldAllocParams->second);
  }

  if (newAllocParams.has_value()) {
    sample.addString(
        "new_seed_prefix",
        folly::IPAddress::networkToString(newAllocParams->first));
    sample.addInt("new_alloc_len", newAllocParams->second);
  }

  logSampleQueue_.push(sample);
}

} // namespace openr
