/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/fibers/FiberManagerMap.h>
#include <openr/common/OpenrEventBase.h>

namespace openr {

namespace {
folly::fibers::FiberManager::Options
getFmOptions() {
  folly::fibers::FiberManager::Options options;
  // NOTE: We use higher stack size (256KB per fiber)
  options.stackSize = 256 * 1024;
  return options;
}
} // namespace

EventBaseStopSignalHandler::EventBaseStopSignalHandler(folly::EventBase* evb)
    : folly::AsyncSignalHandler(evb) {
  registerSignalHandler(SIGINT);
  registerSignalHandler(SIGQUIT);
  registerSignalHandler(SIGTERM);
}

void
EventBaseStopSignalHandler::signalReceived(int signal) noexcept {
  LOG(INFO) << "Caught signal: " << signal << ". Stopping openr event-base...";
  getEventBase()->terminateLoopSoon();
  LOG(INFO) << "Openr event-base stopped";
}

OpenrEventBase::OpenrEventHandler::OpenrEventHandler(
    folly::EventBase* evb, int fd, int events, SocketCallback callback)
    : folly::EventHandler(evb, folly::NetworkSocket::fromFd(fd)),
      callback_(std::move(callback)),
      events_(events) {
  // Make sure evb is not nullptr
  CHECK(evb);

  // Register handler
  registerHandler(folly::EventHandler::PERSIST | events_);
}

void
OpenrEventBase::OpenrEventHandler::handlerReady(uint16_t events) noexcept {
  // Invoke callback if there is an overlap
  if (events & events_) {
    callback_(events);
  }
}

OpenrEventBase::OpenrEventBase()
    : fiberManager_(folly::fibers::getFiberManager(evb_, getFmOptions())) {
  // Periodic timer to update eventbase's timestamp. This is used by Watchdog to
  // identify stuck threads.
  // update aliveness timestamp
  timestamp_.store(std::chrono::steady_clock::now().time_since_epoch().count());
  timeout_ = folly::AsyncTimeout::make(evb_, [this]() noexcept {
    timestamp_.store(
        std::chrono::steady_clock::now().time_since_epoch().count());
    timeout_->scheduleTimeout(std::chrono::seconds(1));
  });
  timeout_->scheduleTimeout(0);
}

OpenrEventBase::~OpenrEventBase() {}

void
OpenrEventBase::run() {
  evb_.loopForever();
}

void
OpenrEventBase::stop() {
  for (auto& future : fiberTaskFutures_) {
    future.wait();
  }
  evb_.terminateLoopSoon();
}

bool
OpenrEventBase::isRunning() const {
  return evb_.isRunning();
}

void
OpenrEventBase::waitUntilRunning() {
  evb_.waitUntilRunning();
}

void
OpenrEventBase::waitUntilStopped() {
  while (isRunning()) {
    std::this_thread::yield();
  }
}

void
OpenrEventBase::scheduleTimeout(
    std::chrono::milliseconds timeout, folly::EventBase::Func callback) {
  evb_.scheduleAt(
      std::move(callback), timeout + std::chrono::steady_clock::now());
}

void
OpenrEventBase::scheduleTimeoutAt(
    std::chrono::steady_clock::time_point scheduleTime,
    folly::EventBase::Func callback) {
  evb_.scheduleAt(std::move(callback), scheduleTime);
}

void
OpenrEventBase::addSocketFd(int socketFd, int events, SocketCallback callback) {
  if (fdHandlers_.count(socketFd)) {
    throw std::runtime_error("Socket-fd is already registered");
  }
  fdHandlers_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(socketFd),
      std::forward_as_tuple(&evb_, socketFd, events, std::move(callback)));
}

void
OpenrEventBase::removeSocketFd(int socketFd) {
  fdHandlers_.erase(socketFd);
}

} // namespace openr
