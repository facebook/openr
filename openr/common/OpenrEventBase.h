/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <csignal>

#include <folly/fibers/FiberManager.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/EventHandler.h>

namespace openr {

using SocketCallback = folly::Function<void(uint16_t revents) noexcept>;

class EventBaseStopSignalHandler : public folly::AsyncSignalHandler {
 public:
  explicit EventBaseStopSignalHandler(folly::EventBase* evb);

 protected:
  void signalReceived(int signum) noexcept override;
};

class OpenrEventBase {
 public:
  OpenrEventBase();

  virtual ~OpenrEventBase();

  /**
   * Get pointer to underlying event-base
   */

  folly::EventBase*
  getEvb() {
    return &evb_;
  }

  /**
   * Add a task to fiber manager. All tasks will be awaited in `stop()`.
   */
  template <typename F>
  void
  addFiberTask(F&& func) {
    fiberTaskFutures_.emplace_back(
        fiberManager_.addTaskFuture(std::move(func)));
  }

  /**
   * Another flavor of adding a task to fiber manager. But user will be
   * responsible to wait for the fiber completion in termination sequence.
   */
  template <typename F>
  folly::Future<folly::Unit>
  addFiberTaskFuture(F&& func) {
    return fiberManager_.addTaskFuture(std::move(func));
  }

  /**
   * EventBase API aliases
   */
  void
  runInEventBaseThread(folly::EventBase::Func callback) {
    evb_.runInEventBaseThread(std::move(callback));
  }

  /**
   * Get latest timestamp of health check timer
   */
  std::chrono::steady_clock::time_point
  getTimestamp() const noexcept {
    return std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(timestamp_.load()));
  }

  /**
   * Runnable interface APIs
   */

  virtual void run();

  virtual void stop();

  bool isRunning() const;

  void waitUntilRunning();

  void waitUntilStopped();

  /**
   * Timeout APIs
   */

  void scheduleTimeout(
      std::chrono::milliseconds timeout, folly::EventBase::Func callback);

  void scheduleTimeoutAt(
      std::chrono::steady_clock::time_point scheduleTime,
      folly::EventBase::Func callback);

  /**
   * Socket/FD polling APIs
   */

  void addSocketFd(int socketFd, int events, SocketCallback callback);
  void removeSocketFd(int socketFd);

  /**
   * Eventbase name related APIs
   */

  std::string
  getEvbName() {
    return evbName_;
  }

  void
  setEvbName(std::string name) {
    evbName_ = name;
  }

 private:
  /**
   * Event handler class for sockets and fds
   */
  class OpenrEventHandler : public folly::EventHandler {
   public:
    OpenrEventHandler(
        folly::EventBase* evb, int fd, int events, SocketCallback callback);

    virtual ~OpenrEventHandler() override {}

   private:
    // EventHandler callback. Unblocks read/write wait
    void handlerReady(uint16_t events) noexcept override;

    // Callback for handling event
    SocketCallback callback_;

    // Subscribed events
    const int events_{0};
  };

  // EventBase object for async event polling/scheduling
  folly::EventBase evb_;

  // FiberManager driven by evb_, for scheduling fiber tasks
  folly::fibers::FiberManager& fiberManager_;
  std::vector<folly::Future<folly::Unit>> fiberTaskFutures_;

  // Data structure to hold fd and their handlers
  std::unordered_map<int /* fd */, OpenrEventHandler> fdHandlers_;

  // Timestamp
  std::atomic<std::chrono::steady_clock::duration::rep> timestamp_;
  std::unique_ptr<folly::AsyncTimeout> timeout_;

  // Unique name to identify eventbase
  std::string evbName_;
};

} // namespace openr
