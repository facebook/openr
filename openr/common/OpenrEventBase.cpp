/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/OpenrEventBase.h"

#include <folly/fibers/FiberManagerMap.h>

namespace openr {

namespace {
std::chrono::seconds
getElapsedSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch());
}

int
getZmqSocketFd(uintptr_t socketPtr) {
  int socketFd{-1};
  size_t fdLen = sizeof(socketFd);
  const auto rc = zmq_getsockopt(
      reinterpret_cast<void*>(socketPtr), ZMQ_FD, &socketFd, &fdLen);
  CHECK_EQ(0, rc) << "Can't get fd for socket. " << fbzmq::Error();
  return socketFd;
}

folly::fibers::FiberManager::Options
getFmOptions() {
  folly::fibers::FiberManager::Options options;
  // NOTE: We use higher stack size (256KB per fiber)
  options.stackSize = 256 * 1024;
  return options;
}
} // namespace

EventBaseStopSignalHandler::EventBaseStopSignalHandler(folly::EventBase* evb)
    : folly::AsyncSignalHandler(evb) {}

void
EventBaseStopSignalHandler::signalReceived(int signal) noexcept {
  LOG(INFO) << "Caught signal: " << signal << ". Stopping openr event-base...";
  getEventBase()->terminateLoopSoon();
}

OpenrEventBase::ZmqEventHandler::ZmqEventHandler(
    folly::EventBase* evb,
    int fd,
    uintptr_t socketPtr,
    int zmqEvents,
    fbzmq::SocketCallback callback)
    : folly::EventHandler(evb, folly::NetworkSocket::fromFd(fd)),
      callback_(std::move(callback)),
      zmqEvents_(zmqEvents),
      ptr_(reinterpret_cast<void*>(socketPtr)) {
  CHECK(evb);
  // Register handler
  uint16_t events{folly::EventHandler::PERSIST};
  if (zmqEvents & ZMQ_POLLIN) {
    events |= folly::EventHandler::READ;
  }
  if (zmqEvents & ZMQ_POLLOUT) {
    events |= folly::EventHandler::WRITE;
  }
  registerHandler(events);

  // ZMQ is edge triggerred. In some cases zmq fd doesn't trigger read events if
  // message is already pending to read before socket is registered for polling.
  // To avoid such corner cases, we explicitly perform check to read pending
  // events.
  if (ptr_) {
    timeout_ = folly::AsyncTimeout::schedule(
        std::chrono::seconds(0), *evb, [&]() noexcept {
          // Invoke handler to process already pending events if any
          handlerReady(0); // events will be read by zmq_getsockopt
        });
  }
}

void
OpenrEventBase::ZmqEventHandler::handlerReady(uint16_t events) noexcept {
  int zmqEvents{0};
  size_t zmqEventsLen = sizeof(zmqEvents);

  if (ptr_) {
    // ZMQ Socket - Read events via zmq_getsockopt
    auto err = zmq_getsockopt(ptr_, ZMQ_EVENTS, &zmqEvents, &zmqEventsLen);
    CHECK_EQ(0, err) << "Got error while reading events from zmq socket";
  } else {
    // Usual socket/event fd - Use signalled events
    if (events & folly::EventHandler::READ) {
      zmqEvents |= ZMQ_POLLIN;
    }
    if (events & folly::EventHandler::WRITE) {
      zmqEvents |= ZMQ_POLLOUT;
    }
  }

  // Return if no events
  if (not zmqEvents) {
    return;
  }

  do {
    // Invoke callback if there is an overlap
    if (zmqEvents_ & zmqEvents) {
      callback_(zmqEvents);
    }

    if (ptr_ and (zmqEvents & ZMQ_POLLIN)) {
      // Get socket events after the read
      auto err = zmq_getsockopt(ptr_, ZMQ_EVENTS, &zmqEvents, &zmqEventsLen);
      CHECK_EQ(0, err) << "Got error while reading events from zmq socket";
    } else {
      zmqEvents = 0;
    }
    // We loop again only for ZMQ_POLLIN events
  } while (zmqEvents & ZMQ_POLLIN);
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
OpenrEventBase::addSocketFd(
    int socketFd, int events, fbzmq::SocketCallback callback) {
  if (fdHandlers_.count(socketFd)) {
    throw std::runtime_error("Socket-fd is already registered");
  }
  fdHandlers_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(socketFd),
      std::forward_as_tuple(
          &evb_,
          socketFd,
          reinterpret_cast<uintptr_t>(nullptr),
          events,
          std::move(callback)));
}

void
OpenrEventBase::addSocket(
    uintptr_t socketPtr, int events, fbzmq::SocketCallback callback) {
  int socketFd = getZmqSocketFd(socketPtr);
  if (fdHandlers_.count(socketFd)) {
    throw std::runtime_error("Socket is already registered");
  }
  fdHandlers_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(socketFd),
      std::forward_as_tuple(
          &evb_, socketFd, socketPtr, events, std::move(callback)));
}

void
OpenrEventBase::removeSocketFd(int socketFd) {
  fdHandlers_.erase(socketFd);
}

void
OpenrEventBase::removeSocket(uintptr_t socketPtr) {
  fdHandlers_.erase(getZmqSocketFd(socketPtr));
}

} // namespace openr
