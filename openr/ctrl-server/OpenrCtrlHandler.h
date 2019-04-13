/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <common/fb303/cpp/FacebookBase2.h>
#include <fbzmq/service/monitor/ZmqMonitorClient.h>
#include <fbzmq/zmq/Zmq.h>
#include <openr/common/OpenrEventLoop.h>
#include <openr/common/Types.h>
#include <openr/if/gen-cpp2/OpenrCtrl.h>

namespace openr {
class OpenrCtrlHandler final : public thrift::OpenrCtrlSvIf,
                               public facebook::fb303::FacebookBase2 {
 public:
  /**
   * NOTE: If acceptablePeerCommonNames is empty then check for peerName is
   *       skipped
   */
  OpenrCtrlHandler(
      const std::string& nodeName,
      const std::unordered_set<std::string>& acceptablePeerCommonNames,
      std::unordered_map<
          thrift::OpenrModuleType,
          std::shared_ptr<OpenrEventLoop>>& moduleTypeToEvl,
      MonitorSubmitUrl const& monitorSubmitUrl,
      fbzmq::Context& context);

  ~OpenrCtrlHandler() override;

  //
  // Raw APIs to directly interact with Open/R modules
  //

  virtual folly::SemiFuture<std::unique_ptr<std::string>> semifuture_command(
      thrift::OpenrModuleType type,
      std::unique_ptr<std::string> request) override;

  virtual folly::SemiFuture<bool> semifuture_hasModule(
      thrift::OpenrModuleType type) override;

  //
  // fb303 service APIs
  //

  facebook::fb303::cpp2::fb_status getStatus() override;

  void getCounters(std::map<std::string, int64_t>& _return) override;
  void getRegexCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::string> regex) override;
  void getSelectedCounters(
      std::map<std::string, int64_t>& _return,
      std::unique_ptr<std::vector<std::string>> keys) override;
  int64_t getCounter(std::unique_ptr<std::string> key) override;

 private:
  void authorizeConnection();
  const std::string nodeName_;
  const std::unordered_set<std::string> acceptablePeerCommonNames_;
  std::unordered_map<thrift::OpenrModuleType, std::shared_ptr<OpenrEventLoop>>
      moduleTypeToEvl_;
  std::unordered_map<
      thrift::OpenrModuleType,
      fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>>
      moduleSockets_;

  // client to interact with monitor
  std::unique_ptr<fbzmq::ZmqMonitorClient> zmqMonitorClient_;

}; // class OpenrCtrlHandler
} // namespace openr
