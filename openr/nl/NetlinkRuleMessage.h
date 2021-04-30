/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/nl/NetlinkMessageBase.h>
#include <openr/nl/NetlinkTypes.h>

extern "C" {
#include <linux/fib_rules.h>
}

namespace openr::fbnl {
/**
 * Message specialization for rtnetlink RULE type
 *
 * For reference: https://man7.org/linux/man-pages/man7/rtnetlink.7.html
 *
 * RTM_NEWRULE, RTM_DELRULE, RTM_GETRULE
 *    Add, delete, or retrieve a routing rule.  Carries a struct
 *    rtmsg
 */

class NetlinkRuleMessage final : public NetlinkMessageBase {
 public:
  NetlinkRuleMessage();

  ~NetlinkRuleMessage() override;

  // Override setReturnStatus. Set rulePromise_ with rcvdRules_
  void setReturnStatus(int status) override;

  // Get future for received rule in response to GET request
  folly::SemiFuture<folly::Expected<std::vector<Rule>, int>>
  getRulesSemiFuture() {
    return rulePromise_.getSemiFuture();
  }

  // initiallize rule message with default params
  void init(int type);

  // parse Netlink Rule message
  static Rule parseMessage(const struct nlmsghdr* nlh);

 private:
  // inherited class implementation
  void rcvdRule(Rule&& rule) override;

  //
  // Private variables for rtnetlink msg exchange
  //

  // pointer to rule message header
  //   struct fib_rule_hdr {
  //     __u8 family;
  //     __u8 dst_len;
  //     __u8 src_len;
  //     __u8 tos;

  //     __u8 table;
  //     __u8 res1; /* reserved */
  //     __u8 res2; /* reserved */
  //     __u8 action;

  //     __u32 flags;
  //   };
  struct fib_rule_hdr* rulehdr_{nullptr};

  // promise to be fulfilled when receiving kernel reply
  folly::Promise<folly::Expected<std::vector<Rule>, int>> rulePromise_;
  std::vector<Rule> rcvdRules_;
};

} // namespace openr::fbnl
