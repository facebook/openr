/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include <openr/common/OpenrEventBase.h>
#include <openr/common/Util.h>
#include <openr/ctrl-server/OpenrCtrlHandler.h>
#include <openr/fib/Fib.h>
#include <openr/watchdog/Watchdog.h>
#include "common/fb303/cpp/DefaultMonitor.h"

namespace openr {

bool waitForFibService(const folly::EventBase& signalHandlerEvb, int port);

/**
 * Start an EventBase in a thread, maintain order of thread creation and
 * returns raw pointer of Derived class.
 */
template <typename T>
T*
startEventBase(
    std::vector<std::thread>& allThreads,
    std::vector<std::unique_ptr<OpenrEventBase>>& orderedEvbs,
    Watchdog* watchdog,
    const std::string& name,
    std::unique_ptr<T> evbT) {
  CHECK(evbT);
  auto t = evbT.get();
  auto evb = std::unique_ptr<OpenrEventBase>(
      reinterpret_cast<OpenrEventBase*>(evbT.release()));
  evb->setEvbName(name);

  // Start a thread
  allThreads.emplace_back(std::thread([evb = evb.get(), name]() noexcept {
    XLOG(INFO) << fmt::format("Starting {} thread ...", name);
    folly::setThreadName(name);
    evb->run();
    XLOG(INFO) << fmt::format("[Exit] Successfully stopped {} thread.", name);
  }));
  evb->waitUntilRunning();

  // Add to watchdog
  if (watchdog) {
    watchdog->addEvb(evb.get());
  }

  // Emplace evb into ordered list of evbs. So that we can destroy
  // them in revserse order of their creation.
  orderedEvbs.emplace_back(std::move(evb));

  return t;
}

std::shared_ptr<apache::thrift::ThriftServer> setUpThriftServer(
    std::shared_ptr<const Config> config,
    std::shared_ptr<openr::OpenrCtrlHandler>& handler,
    std::shared_ptr<wangle::SSLContextConfig> sslContext);

void waitTillStart(std::shared_ptr<apache::thrift::ThriftServer> server);

} // namespace openr
