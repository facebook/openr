/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/spark/IoProvider.h>

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <folly/IPAddress.h>

namespace openr {

using ConnectedIfPairs = std::map<
    std::string /* ifName */,
    std::vector<std::pair<std::string /* ifName */, int32_t /* latency */>>>;

using IfNameAndifIndex = std::vector<std::pair<std::string, int>>;

/**
 * MockIoProvider glues all Sparks together and emulates the send/recv of
 * Spark hello packets. IoProvider does reall work but this one fakes all the
 * work.
 *
 * At a high level we have a pipe for each Spark. Read-end FD is passed to
 * spark to do polling and write-end is kept by MockIoProvider. Whenever a
 * message is ready to be delivered to spark then MockIoProvider signals to
 * Spark by writing a signal byte onto the pipe which breaks poll on Spark
 * side. Spark then reads the message from IoProvider.
 *
 * When a Spark wants to multicast/send message then it is put onto the mailbox
 * of every other connected spark with specified delay.
 *
 * MockIoProvider thread constantly keep checking mailboxes and if any message
 * becomes active then signals it to Spark by writing a byte onto pipe.
 */
class MockIoProvider final : public IoProvider {
 public:
  MockIoProvider() = default;

  void
  start() {
    isRunning_.store(true, std::memory_order_relaxed);
    while (isRunning_.load(std::memory_order_relaxed)) {
      processMailboxes();
      std::this_thread::yield();
    }
  }

  void
  stop() {
    isRunning_.store(false, std::memory_order_relaxed);
  }

  /**
   * Busy spin until the zmq thread is running. this is called from external
   * threads to synchronize with this thread's main loop
   */
  void
  waitUntilRunning() const {
    while (!isRunning_.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
  }

  // This is invoked periodically by MockIoProvider thread. It checks for
  // messages and if they are active then sends them onto pipe for appropriate
  // spark-thread to consume with kernel timestamp.
  void processMailboxes();

  // define interface pairs which are connected. an interface will send
  // packets to all connected interfaces. E.g. if we have x-> y, z then
  // packet sent off of x will be delivered to y, z
  void setConnectedPairs(ConnectedIfPairs connectedIfPairs);

  //
  // The usual IO jazz
  //

  int socket(int domain, int type, int protocol) override;

  int fcntl(int fd, int cmd, int arg) override;

  int bind(
      int sockfd, const struct sockaddr* my_addr, socklen_t addrlen) override;

  ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) override;

  ssize_t sendmsg(int sockfd, const struct msghdr* msg, int flags) override;

  int setsockopt(
      int sockfd,
      int level,
      int optname,
      const void* optval,
      socklen_t optlen) override;

  //
  // User provides us a mapping of interface names and ifIndex pairs
  //
  void addIfNameIfIndex(const IfNameAndifIndex& entries);

 private:
  // Boolean to keep track of running-state of MockIoProvider
  std::atomic<bool> isRunning_{false};

  // used to make this class a monitor
  std::mutex mutex_{};

  ConnectedIfPairs connectedIfPairs_{};

  // Map of send/recv fds. All fds used below belong to recv-fd which is being
  // polled by Spark (or returned to spark).
  std::map<int /* recv-fd */, int /* send-fd */> pipeFds_;

  // the mapping from fd to the interface name that owns it
  std::map<int, std::string /* ifName */> fdToIfName_;

  // map from allocated indexes to names and reverse
  std::map<int /* ifIndex*/, std::string /* ifName */> ifIndexToIfName_{};

  std::map<std::string /* ifName */, int /* ifIndex */> ifNameToIfIndex_{};

  // maps the fds that have joined the interface: we can have same fd
  // joining on multiple interfaces
  std::map<int /* ifIndex */, int /* fd */> ifIndexToFd_{};

  struct IoMessage {
    IoMessage(
        int ifIndex,
        folly::IPAddress srcAddr,
        std::string data,
        std::chrono::milliseconds delay)
        : ifIndex(ifIndex),
          srcAddr(srcAddr),
          data(std::move(data)),
          deliveryTime(std::chrono::steady_clock::now() + delay) {}

    // This this message ready to be sent
    bool
    isActive() {
      return std::chrono::steady_clock::now() > deliveryTime;
    }

    // interface index to which this message has to be delivered.
    const int ifIndex{0};

    // IPAddress of the sender
    const folly::IPAddress srcAddr;

    // Raw message received from sender
    const std::string data;

    // Time point when this message needs to be delivered.
    const std::chrono::steady_clock::time_point deliveryTime;

    // Have we sent a ping for this message to Spark via pipe ? This boolean
    // flag helps avoiding sending duplicate pings.
    bool clientNotified{false};
  };

  // the list of messages pending per fd
  std::map<int /* fd */, std::list<IoMessage>> mailboxes_{};
};
} // namespace openr
