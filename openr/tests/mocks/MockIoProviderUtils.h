/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/tests/mocks/MockIoProvider.h>

#include <openr/spark/IoProvider.h>

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <folly/IPAddress.h>

namespace openr {
namespace MockIoProviderUtils {

template <typename T>
union AlignedCtrlBuf {
  char cbuf[CMSG_SPACE(sizeof(T))];
  struct cmsghdr align;
};

// Allows for a form of named-parameters in the caller.
struct networkArgs {
  const int srcIfIndex;
  const folly::IPAddress& srcIPAddr;
  const folly::IPAddress& dstIPAddr;
  const int dstPort;
  sockaddr_storage& dstAddrStorage;
};

template <typename T>
struct bufferArgs {
  struct msghdr& msg;
  void* data;
  size_t len;
  struct iovec& entry;
  AlignedCtrlBuf<T>& u;
};

template <typename T>
void prepareSendMessage(bufferArgs<T> bufargs, struct networkArgs netargs);

template <typename T>
void prepareRecvMessage(
    bufferArgs<T> bufargs, sockaddr_storage& srcAddrStorage);

int getMsgIfIndex(struct msghdr* msg);

int createSocketAndJoinGroup(
    std::shared_ptr<openr::MockIoProvider> mockIoProvider,
    int ifIndex,
    folly::IPAddress /* mcastGroup */);

} // namespace MockIoProviderUtils
} // namespace openr
