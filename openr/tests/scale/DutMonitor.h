/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>

#include <folly/io/async/EventBase.h>

#include <openr/if/gen-cpp2/OpenrCtrlAsyncClient.h>

namespace openr {

/*
 * DutMonitor queries general DUT state via thrift (fb303 counters, etc.).
 *
 * This separates non-KvStore DUT queries from KvStoreThriftInjector,
 * which is focused on topology injection. DutMonitor owns its own
 * thrift connection to the DUT.
 */
class DutMonitor {
 public:
  /*
   * Construct monitor with DUT connection info.
   *
   * @param dutHost DUT hostname or IP address
   * @param dutPort DUT OpenR thrift port (default: 2018)
   */
  explicit DutMonitor(const std::string& dutHost, uint16_t dutPort = 2018);

  ~DutMonitor();

  /*
   * Connect to the DUT.
   *
   * @return true if connection successful
   */
  bool connect();

  /*
   * Disconnect from the DUT.
   */
  void disconnect();

  /*
   * Check if connected to the DUT.
   */
  bool isConnected() const;

  /*
   * Get fb303 counters matching a regex pattern.
   *
   * @param regex Pattern to filter counters (e.g., "kvstore\\..*")
   * @return Map of matching counter name to value
   */
  std::map<std::string, int64_t> getRegexCounters(const std::string& regex);

 private:
  std::string dutHost_;
  uint16_t dutPort_;

  std::unique_ptr<folly::EventBase> evb_;
  std::unique_ptr<apache::thrift::Client<thrift::OpenrCtrl>> client_;

  bool connected_{false};
};

} // namespace openr
