/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/OpenrModule.h>

namespace openr {

class OpenrEventBase : public OpenrModule {
 public:
  OpenrEventBase(
      const std::string& nodeName,
      const thrift::OpenrModuleType type,
      fbzmq::Context& zmqContext);

  /**
   * Default constructor for using polling/timeout functionality. Module
   * functionality will be disabled by default
   */
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
   * EventBase API aliases
   */
  void
  runInEventBaseThread(folly::EventBase::Func callback) {
    evb_.runInEventBaseThread(std::move(callback));
  }

  /**
   * OpenrModule APIs
   */
  std::chrono::seconds
  getTimestamp() const override {
    return timestamp_.load();
  }

  /**
   * Runnable interface APIs
   */

  void run() override;

  void stop() override;

  bool isRunning() const override;

  void waitUntilRunning() override;

  void waitUntilStopped() override;

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

  void addSocketFd(int socketFd, int events, fbzmq::SocketCallback callback);
  void addSocket(
      uintptr_t socketPtr, int events, fbzmq::SocketCallback callback);

  void removeSocketFd(int socketFd);
  void removeSocket(uintptr_t socketPtr);

 protected:
  // Default implementation for `processRequestMsg` to FATAL if invoked.
  folly::Expected<fbzmq::Message, fbzmq::Error>
  processRequestMsg(fbzmq::Message&& /* request */) override {
    LOG(FATAL) << "This method must be implemented by subclass";
  }

 private:
  /**
   * Event handler class for sockets and fds
   */
  class ZmqEventHandler : public folly::EventHandler {
   public:
    ZmqEventHandler(
        folly::EventBase* evb,
        int fd,
        uintptr_t socketPtr,
        int zmqEvents,
        fbzmq::SocketCallback callback);

    virtual ~ZmqEventHandler() {}

   private:
    // EventHandler callback. Unblocks read/write wait
    void handlerReady(uint16_t events) noexcept override;

    // Callback for handling event
    fbzmq::SocketCallback callback_;

    // fbzmq socket pointer if fd is socket
    void* ptr_{nullptr};

    // AsyncTimeout for reading first set of events
    std::unique_ptr<folly::AsyncTimeout> timeout_;
  };

  // EventBase object for async event polling/scheduling
  folly::EventBase evb_;

  // Data structure to hold fd and their handlers
  std::unordered_map<int /* fd */, ZmqEventHandler> fdHandlers_;

  // Timestamp
  std::atomic<std::chrono::seconds> timestamp_;
  std::unique_ptr<folly::AsyncTimeout> timeout_;
};

} // namespace openr
