/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PeriodicPinger.h"

#include <cstddef>

#include <linux/in6.h>
#include <netinet/icmp6.h>
#include <netinet/ip6.h>

#include <folly/Format.h>

using namespace openr::fbmeshd;

PeriodicPinger::PeriodicPinger(
    folly::EventBase* evb,
    folly::IPAddressV6 dst,
    folly::IPAddressV6 src,
    std::chrono::milliseconds interval,
    const std::string& interface)
    : dst_{dst}, src_{src}, interface_{interface} {
  periodicPingerTimer_ =
      folly::AsyncTimeout::make(*evb, [this, interval]() noexcept {
        doPing();
        periodicPingerTimer_->scheduleTimeout(interval);
      });
  periodicPingerTimer_->scheduleTimeout(interval);
}

void
PeriodicPinger::doPing() {
  VLOG(8) << folly::sformat("PeriodicPinger::{}()", __func__);
  auto sock = ::socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
  CHECK_NE(sock, -1);

  icmp6_hdr icmpHeader;
  icmpHeader.icmp6_type = ICMP6_ECHO_REQUEST;
  icmpHeader.icmp6_code = 0;

  auto checksumOffset = offsetof(icmp6_hdr, icmp6_cksum);
  CHECK_EQ(
      ::setsockopt(
          sock,
          SOL_RAW,
          IPV6_CHECKSUM,
          &checksumOffset,
          sizeof(checksumOffset)),
      0);

  const auto dstSockAddr = dst_.toSockAddr();

  ::sendto(
      sock,
      &icmpHeader,
      sizeof(icmpHeader),
      0,
      const_cast<sockaddr*>(reinterpret_cast<const sockaddr*>(&dstSockAddr)),
      sizeof(dstSockAddr));

  ::close(sock);
}
