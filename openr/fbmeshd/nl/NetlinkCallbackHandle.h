/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/handlers.h> // @manual
#include <netlink/socket.h> // @manual

#include <functional>

#include <glog/logging.h>

#include <openr/fbmeshd/nl/NetlinkMessage.h>

namespace openr {
namespace fbmeshd {

/**
 * Netlink callback handle class, can be used to wrap different callbacks
 * associated with a netlink call
 */
template <typename MessageType = NetlinkMessage>
class NetlinkCallbackHandle {
 public:
  using CallbackFn = std::function<int(const MessageType&)>;
  using ErrorCallbackFn = std::function<int(sockaddr_nl*, nlmsgerr*)>;

  NetlinkCallbackHandle(nl_cb_kind kind = NL_CB_DEFAULT)
      : cb_{nl_cb_alloc(kind)} {
    CHECK_NOTNULL(cb_);
  }

  NetlinkCallbackHandle(nl_sock* sock) : cb_{nl_socket_get_cb(sock)} {
    CHECK_NOTNULL(cb_);
  }

  ~NetlinkCallbackHandle() {
    nl_cb_put(cb_);
  }

  NetlinkCallbackHandle(const NetlinkCallbackHandle&) = delete;

  void
  setErrorCallback(ErrorCallbackFn cbError) {
    cbError_ = cbError;
    CHECK_EQ(nl_cb_err(cb_, NL_CB_CUSTOM, &customErrorCallback, &cbError_), 0);
  }

  void
  setValidCallback(CallbackFn cbValid) {
    cbValid_ = cbValid;
    setCallback<NL_CB_VALID>(cbValid_);
  }

  void
  setFinishCallback(CallbackFn cbFinish) {
    cbFinish_ = cbFinish;
    setCallback<NL_CB_FINISH>(cbFinish_);
  }

  void
  setAckCallback(CallbackFn cbAck) {
    cbAck_ = cbAck;
    setCallback<NL_CB_ACK>(cbAck_);
  }

  explicit operator nl_cb*() const {
    return cb_;
  };

 private:
  template <nl_cb_type type>
  void
  setCallback(CallbackFn& cb) {
    CHECK_EQ(nl_cb_set(cb_, type, NL_CB_CUSTOM, &customCallback, &cb), 0);
  }

  static int
  customCallback(nl_msg* msg, void* arg) {
    return (*static_cast<CallbackFn*>(arg))(MessageType{msg});
  }

  static int
  customErrorCallback(sockaddr_nl* nla, nlmsgerr* err, void* arg) {
    return (*static_cast<ErrorCallbackFn*>(arg))(nla, err);
  }

  ErrorCallbackFn cbError_;
  CallbackFn cbValid_;
  CallbackFn cbFinish_;
  CallbackFn cbAck_;
  nl_cb* cb_;
};

} // namespace fbmeshd
} // namespace openr
