/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/msg.h> // @manual
#include <netlink/netlink.h> // @manual

#include <glog/logging.h>

#include <folly/Singleton.h>

#include <openr/fbmeshd/nl/NetlinkCallbackHandle.h>

namespace openr {
namespace fbmeshd {

/**
 * Netlink wrapper for a nl_sock
 */
template <typename NetlinkMessageType = NetlinkMessage>
class NetlinkSocket {
 public:
  NetlinkSocket() : sock_{nl_socket_alloc()}, cb_{sock_} {
    CHECK_NOTNULL(sock_);
  }

  ~NetlinkSocket() {
    nl_socket_free(sock_);
  }

  void
  connect(int protocol) {
    int err = nl_connect(sock_, protocol);
    CHECK_EQ(err, 0) << nl_geterror(err);
  }

  void
  sendAndReceive(
      const NetlinkMessageType& msg,
      const std::function<int(const NetlinkMessageType&)>& cbValid) const {
    send(msg);
    receive(cbValid);
  }

  void
  sendAndReceive(const NetlinkMessageType& msg) const {
    sendAndReceive(msg, [](const auto&) { return NL_OK; });
  }

  void
  send(const NetlinkMessageType& msg) const {
    int err;

    // Send
    do {
      err = nl_send_auto(sock_, msg);
    } while (err == -NLE_INTR);
    CHECK_GE(err, 0) << nl_geterror(err);

    const auto msgHeader = msg.getHeader();
    genlmsghdr* genlmsgHeader = static_cast<genlmsghdr*>(nlmsg_data(msgHeader));
    VLOG(10) << "Sent nl message (command: "
             << static_cast<uint32_t>(genlmsgHeader->cmd)
             << "; sequence: " << msgHeader->nlmsg_seq << ")";
  }

  void
  receive(const std::function<int(const NetlinkMessageType&)>& cbValid) const {
    NetlinkCallbackHandle<NetlinkMessageType> cb;

    int ret{1};

    cb.setErrorCallback([&ret](sockaddr_nl*, nlmsgerr* err) {
      genlmsghdr* gnlh = static_cast<genlmsghdr*>(nlmsg_data(&err->msg));
      ret = err->error;

      if (err->error == -NLE_INTR) {
        VLOG(10)
            << "Interrupted syscall error; retrying receiving netlink message ("
            << "command: " << static_cast<uint32_t>(gnlh->cmd)
            << "; sequence: " << err->msg.nlmsg_seq << ")";
        return NL_SKIP;
      }

      if (err->error == -NLE_MSG_OVERFLOW) {
        LOG(WARNING)
            << "Kernel overflow error; retrying receiving netlink message. ("
            << "command: " << static_cast<uint32_t>(gnlh->cmd)
            << "; sequence: " << err->msg.nlmsg_seq << ")";
        return NL_SKIP;
      }

      throw std::runtime_error(
          "Netlink error (" + std::string{nl_geterror(err->error)} + ")" +
          "command: " + std::to_string(static_cast<uint32_t>(gnlh->cmd)) +
          "; sequence: " + std::to_string(err->msg.nlmsg_seq));
    });

    cb.setFinishCallback([&ret](const NetlinkMessage&) {
      ret = 0;
      return NL_SKIP;
    });

    cb.setAckCallback([&ret](const NetlinkMessage&) {
      ret = 0;
      return NL_STOP;
    });

    cb.setValidCallback(cbValid);

    while (ret > 0) {
      // This is a blocking call, and the above callbacks will be invoked until
      // one of them returns NL_STOP or NL_OK.
      receive(cb);
    }
  }

  template <typename CallbackHandleType>
  void
  receive(const CallbackHandleType& cb) const {
    const auto err = tryReceive(cb);
    CHECK_EQ(err, 0) << nl_geterror(err);
  }

  template <typename CallbackHandleType>
  int
  tryReceive(const CallbackHandleType& cb) const {
    int err;
    do {
      err = nl_recvmsgs(sock_, static_cast<nl_cb*>(cb));
    } while (err == -NLE_INTR || err == -NLE_DUMP_INTR);
    return err;
  }

  void
  setBufferSize(int rxbuf, int txbuf) const {
    int err = nl_socket_set_buffer_size(sock_, rxbuf, txbuf);
    CHECK_EQ(err, 0) << nl_geterror(err);
  }

  void
  disableSequenceChecking() const {
    nl_socket_disable_seq_check(sock_);
  }

  FOLLY_NODISCARD int
  getFd() const {
    int fd = nl_socket_get_fd(sock_);
    CHECK_NE(fd, -1);
    return fd;
  }

  FOLLY_NODISCARD NetlinkCallbackHandle<NetlinkMessageType>&
  getCallbackHandle() {
    return cb_;
  }

  void
  addMembership(int group) {
    int err = nl_socket_add_membership(sock_, group);
    CHECK_EQ(err, 0) << nl_geterror(err);
  }

 protected:
  nl_sock* sock_;
  NetlinkCallbackHandle<NetlinkMessageType> cb_;
};

} // namespace fbmeshd
} // namespace openr
