/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/nl/NetlinkRuleMessage.h>

namespace openr::fbnl {
NetlinkRuleMessage::NetlinkRuleMessage() : NetlinkMessageBase() {}

NetlinkRuleMessage::~NetlinkRuleMessage() {
  CHECK(rulePromise_.isFulfilled());
}

void
NetlinkRuleMessage::rcvdRule(Rule&& rule) {
  rcvdRules_.emplace_back(std::move(rule));
}

void
NetlinkRuleMessage::setReturnStatus(int status) {
  if (status == 0) {
    rulePromise_.setValue(std::move(rcvdRules_));
  } else {
    rulePromise_.setValue(folly::makeUnexpected(status));
  }
  NetlinkMessageBase::setReturnStatus(status);
}

void
NetlinkRuleMessage::init(int type) {
  if (type != RTM_NEWRULE && type != RTM_DELRULE && type != RTM_GETRULE) {
    LOG(ERROR) << "Incorrect Netlink message type";
    return;
  }

  // initialize netlink header
  msghdr_->nlmsg_len = NLMSG_LENGTH(sizeof(struct fib_rule_hdr));
  msghdr_->nlmsg_type = type;
  msghdr_->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

  if (type == RTM_GETRULE) {
    // Get all rules
    msghdr_->nlmsg_flags |= NLM_F_DUMP;
  }

  if (type == RTM_NEWRULE) {
    // We create new rule or replace existing
    msghdr_->nlmsg_flags |= NLM_F_CREATE;
    msghdr_->nlmsg_flags |= NLM_F_REPLACE;
  }

  // intialize the rule message header
  auto nlmsgAlen = NLMSG_ALIGN(sizeof(struct nlmsghdr));
  rulehdr_ = reinterpret_cast<struct fib_rule_hdr*>((char*)msghdr_ + nlmsgAlen);
}

Rule
NetlinkRuleMessage::parseMessage(const struct nlmsghdr* nlmsg) {
  const struct fib_rule_hdr* const ruleEntry =
      reinterpret_cast<struct fib_rule_hdr*>(NLMSG_DATA(nlmsg));

  const uint16_t family = ruleEntry->family;
  const uint8_t action = ruleEntry->action;
  // table is uint8_t
  const uint32_t table = static_cast<uint32_t>(ruleEntry->table);
  // construct rule
  Rule rule(family, action, table);

  const struct rtattr* ruleAttr;
  auto ruleAttrLen = RTM_PAYLOAD(nlmsg);
  // process all rule attributes
  for (ruleAttr = RTM_RTA(ruleEntry); RTA_OK(ruleAttr, ruleAttrLen);
       ruleAttr = RTA_NEXT(ruleAttr, ruleAttrLen)) {
    switch (ruleAttr->rta_type) {
    case FRA_FWMARK: {
      rule.setFwmark(*(reinterpret_cast<uint32_t*> RTA_DATA(ruleAttr)));
    } break;
    case FRA_TABLE: {
      rule.setTable(*(reinterpret_cast<uint32_t*> RTA_DATA(ruleAttr)));
    } break;
    case FRA_PRIORITY: {
      rule.setPriority(*(reinterpret_cast<uint32_t*> RTA_DATA(ruleAttr)));
    } break;
    }
  }

  VLOG(3) << "Netlink parsed rule message. " << rule.str();
  return rule;
}

int
NetlinkRuleMessage::addRule(const Rule& rule) {
  init(RTM_NEWRULE);

  return addRuleAttributes(rule);
}

int
NetlinkRuleMessage::deleteRule(const Rule& rule) {
  init(RTM_DELRULE);

  return addRuleAttributes(rule);
}

int
NetlinkRuleMessage::addRuleAttributes(const Rule& rule) {
  int status{0};

  const uint32_t table = rule.getTable();
  // set rulehdr fields
  rulehdr_->table = table < 256 ? table : RT_TABLE_COMPAT;
  rulehdr_->action = rule.getAction();
  rulehdr_->family = rule.getFamily();

  // add attributes
  if ((status = addAttributes(
           FRA_TABLE,
           reinterpret_cast<const char*>(&table),
           sizeof(uint32_t)))) {
    return status;
  }
  if (rule.getFwmark()) {
    const uint32_t fwmark = rule.getFwmark().value();
    if ((status = addAttributes(
             FRA_FWMARK,
             reinterpret_cast<const char*>(&fwmark),
             sizeof(uint32_t)))) {
      return status;
    }
  }
  if (rule.getPriority()) {
    const uint32_t priority = rule.getPriority().value();
    if ((status = addAttributes(
             FRA_PRIORITY,
             reinterpret_cast<const char*>(&priority),
             sizeof(uint32_t)))) {
      return status;
    }
  }

  return status;
}

} // namespace openr::fbnl
