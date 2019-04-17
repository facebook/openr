/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Portability.h>
#include <folly/futures/Future.h>

#include <openr/fbmeshd/802.11s/Nl80211Handler.h>
#include <openr/fbmeshd/if/gen-cpp2/MeshService.h>
#include <openr/fbmeshd/openr-metric-manager/OpenRMetricManager.h>
#include <openr/fbmeshd/routing/Routing.h>

namespace openr {
namespace fbmeshd {

class MeshServiceHandler final : public thrift::MeshServiceSvIf {
  // This class should never be copied; remove default copy/move
  MeshServiceHandler() = delete;
  MeshServiceHandler(const MeshServiceHandler&) = delete;
  MeshServiceHandler(MeshServiceHandler&&) = delete;
  MeshServiceHandler& operator=(const MeshServiceHandler&) = delete;
  MeshServiceHandler& operator=(MeshServiceHandler&&) = delete;

 private:
  fbzmq::ZmqEventLoop& evl_;
  Nl80211Handler& nlHandler_;
  OpenRMetricManager* openRMetricManager_;
  Routing* routing_;

 public:
  MeshServiceHandler(
      fbzmq::ZmqEventLoop& evl,
      Nl80211Handler& nlHandler,
      OpenRMetricManager* openRMetricManager,
      Routing* routing)
      : evl_(evl),
        nlHandler_(nlHandler),
        openRMetricManager_(openRMetricManager),
        routing_(routing) {}

  ~MeshServiceHandler() override {}

  template <typename _returnType>
  void
  serviceFunc(
      _returnType& returnVal,
      std::unique_ptr<std::string>& ifNamePtr,
      std::function<bool(std::string&, _returnType*)> nlhfp,
      const std::string& errMsg) {
    folly::Promise<_returnType> promise;
    auto future = promise.getFuture();
    std::string ifName = *ifNamePtr;
    evl_.runInEventLoop(
        [promise = std::move(promise), ifName, nlhfp, errMsg]() mutable {
          try {
            _returnType tempRet;
            bool success = nlhfp(ifName, &tempRet);
            if (!success) {
              throw(thrift::MeshServiceError(errMsg));
            }
            promise.setValue(tempRet);
          } catch (const std::exception& ex) {
            promise.setException(ex);
          }
        });

    try {
      auto result = std::move(future).get();
      returnVal = result;
    } catch (const std::exception& ex) {
      throw(thrift::MeshServiceError(errMsg));
    }
  }

  void
  getPeers(
      std::vector<std::string>& returnVal,
      std::unique_ptr<std::string> ifNamePtr) override {
    serviceFunc<std::vector<std::string>>(
        returnVal,
        ifNamePtr,
        [this](std::string&, std::vector<std::string>* returnVal) {
          for (auto peer : nlHandler_.getPeers()) {
            returnVal->push_back(peer.toString());
          }
          return true;
        },
        "error receiving peer list from netlink");
  }

  void
  getMetrics(
      thrift::PeerMetrics& returnVal,
      std::unique_ptr<std::string> ifNamePtr) override {
    serviceFunc<thrift::PeerMetrics>(
        returnVal,
        ifNamePtr,
        [this](std::string&, thrift::PeerMetrics* returnVal) {
          const auto metrics = nlHandler_.getMetrics();
          std::transform(
              metrics.begin(),
              metrics.end(),
              std::inserter(*returnVal, returnVal->begin()),
              [](const auto& elem) {
                return std::make_pair(elem.first.toString(), elem.second);
              });
          return true;
        },
        "error receiving peer metrics from netlink");
  }

  void
  getMesh(thrift::Mesh& returnVal, std::unique_ptr<std::string> ifNamePtr)
      override {
    serviceFunc<thrift::Mesh>(
        returnVal,
        ifNamePtr,
        [this](std::string&, thrift::Mesh* returnVal) {
          *returnVal = nlHandler_.getMesh();
          return true;
        },
        "error receiving mesh info from netlink");
  }

  void
  setMetricOverride(
      std::unique_ptr<std::string> macAddress, thrift::UInt32 metric) override {
    if (!openRMetricManager_) {
      return;
    }
    try {
      openRMetricManager_->setMetricOverride(
          folly::MacAddress(*macAddress), metric);
    } catch (const std::invalid_argument& e) {
      VLOG(0) << __FUNCTION__ << ": " << e.what();
    }
  }

  FOLLY_NODISCARD thrift::UInt32
  getMetricOverride(std::unique_ptr<std::string> macAddress) override {
    if (!openRMetricManager_) {
      return 0;
    }
    thrift::UInt32 retval = 0;
    try {
      retval = openRMetricManager_->getMetricOverride(
          folly::MacAddress(*macAddress));
    } catch (const std::invalid_argument& e) {
      VLOG(0) << __FUNCTION__ << ": " << e.what();
    }
    return retval;
  }

  int
  clearMetricOverride(std::unique_ptr<std::string> macAddress) override {
    if (!openRMetricManager_) {
      return 0;
    }
    int retval = false;
    if (!macAddress->empty()) {
      try {
        retval = openRMetricManager_->clearMetricOverride(
            folly::MacAddress(*macAddress));
      } catch (const std::invalid_argument& e) {
        VLOG(0) << __FUNCTION__ << ": " << e.what();
      }
    } else {
      // if no MAC address, clear all overrides
      retval = openRMetricManager_->clearMetricOverride();
    }
    return retval;
  }

  void
  dumpMpath(std::vector<thrift::MpathEntry>& ret) override {
    if (!routing_) {
      return;
    }
    auto mpaths = routing_->dumpMpaths();
    for (const auto& it : mpaths) {
      ret.push_back(thrift::MpathEntry{
          apache::thrift::FragileConstructor::FRAGILE,
          it.first.u64NBO(),
          it.second.nextHop.u64NBO(),
          static_cast<int64_t>(it.second.sn),
          static_cast<int32_t>(it.second.metric),
          std::max(
              static_cast<int64_t>(0),
              static_cast<int64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      it.second.expTime - std::chrono::steady_clock::now())
                      .count())),
          static_cast<int8_t>(it.second.flags),
          static_cast<int8_t>(it.second.hopCount),
          it.second.isRoot,
          it.second.isGate,
      });
    }
  }
};
} // namespace fbmeshd
} // namespace openr
