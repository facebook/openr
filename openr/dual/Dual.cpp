/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/dual/Dual.h"

namespace openr {

void
DualStateMachine::processEvent(DualEvent event, bool fc) {
  switch (state) {
  case DualState::PASSIVE: {
    if (fc) {
      return;
    }
    state = event == DualEvent::QUERY_FROM_SUCCESSOR ? DualState::ACTIVE3
                                                     : DualState::ACTIVE1;
    break;
  }
  case DualState::ACTIVE0: {
    if (event != DualEvent::LAST_REPLY) {
      return;
    }
    state = fc ? DualState::PASSIVE : DualState::ACTIVE2;
    break;
  }
  case DualState::ACTIVE1: {
    if (event == DualEvent::INCREASE_D) {
      state = DualState::ACTIVE0;
    } else if (event == DualEvent::LAST_REPLY) {
      state = DualState::PASSIVE;
    } else if (event == DualEvent::QUERY_FROM_SUCCESSOR) {
      state = DualState::ACTIVE2;
    }
    break;
  }
  case DualState::ACTIVE2: {
    if (event != DualEvent::LAST_REPLY) {
      return;
    }
    state = fc ? DualState::PASSIVE : DualState::ACTIVE3;
    break;
  }
  case DualState::ACTIVE3: {
    if (event == DualEvent::LAST_REPLY) {
      state = DualState::PASSIVE;
    } else if (event == DualEvent::INCREASE_D) {
      state = DualState::ACTIVE2;
    }
    break;
  }
  default: {
    LOG(ERROR) << "unknown state";
    break;
  }
  }
}

// class Dual methods

Dual::Dual(
    const std::string& nodeId,
    const std::string& rootId,
    const std::unordered_map<std::string, int64_t>& localDistance)
    : nodeId(nodeId), rootId(rootId), localDistances_(localDistance) {
  // set distance to 0 if I'm the root, otherwise default to inf
  if (rootId == nodeId) {
    info_.distance = 0;
    info_.reportDistance = 0;
    info_.feasibleDistance = 0;
    info_.nexthop = nodeId;
  }
}

int64_t
Dual::getMinDistance() {
  if (nodeId == rootId) {
    // I'm the root
    return 0;
  }
  int64_t dmin = std::numeric_limits<int64_t>::max();
  for (const auto& kv : localDistances_) {
    const auto& ld = kv.second;
    const auto& rd = info_.neighborInfos[kv.first].reportDistance;
    dmin = std::min(dmin, addDistances(ld, rd));
  }
  return dmin;
}

bool
Dual::routeAffected() {
  if (localDistances_.size() == 0) {
    // no neighbor
    return false;
  }

  if (info_.nexthop.hasValue() and *info_.nexthop == nodeId) {
    // my nextHop is myself
    return false;
  }

  auto dmin = getMinDistance();
  if (info_.distance != dmin) {
    // distance changed
    VLOG(2) << rootId << "::" << nodeId << ": distance changed "
            << info_.distance << " -> " << dmin;
    return true;
  }

  if (dmin == std::numeric_limits<int64_t>::max()) {
    // no valid route found
    return false;
  }

  std::unordered_set<std::string> nexthops;
  for (const auto& kv : localDistances_) {
    const auto& neighbor = kv.first;
    const auto& ld = kv.second;
    const auto& rd = info_.neighborInfos[neighbor].reportDistance;
    int64_t d = addDistances(ld, rd);
    if (d == dmin) {
      nexthops.emplace(neighbor);
    }
  }

  if (nexthops.count(*info_.nexthop) == 0) {
    // nextHop changed
    auto oldnh = info_.nexthop.hasValue() ? *info_.nexthop : "none";
    VLOG(2) << rootId << "::" << nodeId << ": nexthop changed " << oldnh
            << " -> " << folly::join(",", nexthops);
    return true;
  }
  return false;
}

bool
Dual::meetFeasibleCondition(std::string& nexthop, int64_t& distance) {
  int64_t dmin = getMinDistance();
  // find feasible nexthop according to SNC(source node condition)
  for (const auto& kv : localDistances_) {
    const auto& neighbor = kv.first;
    const auto& ld = kv.second;
    if (ld == std::numeric_limits<int64_t>::max()) {
      // skip down neighbor
      continue;
    }
    const auto& rd = info_.neighborInfos[neighbor].reportDistance;
    if (rd < info_.feasibleDistance and addDistances(ld, rd) == dmin) {
      VLOG(2) << rootId << "::" << nodeId << ": meet FC: " << neighbor << ", "
              << rd << ", " << dmin;
      nexthop = neighbor;
      distance = dmin;
      return true;
    }
  }
  return false;
}

void
Dual::floodUpdates(
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  thrift::DualMessage msg;
  msg.dstId = rootId;
  msg.distance = info_.reportDistance;
  msg.type = thrift::DualMessageType::UPDATE;

  for (const auto& kv : localDistances_) {
    const auto& neighbor = kv.first;
    if (kv.second == std::numeric_limits<int64_t>::max()) {
      // skip down neighbor
      continue;
    }
    msgsToSend[neighbor].messages.emplace_back(msg);
    counters_[neighbor].updateSent++;
  }
}

void
Dual::localComputation(
    const std::string& newNexthop,
    int64_t newDistance,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  bool sameRd = newDistance == info_.reportDistance;
  // perform local update
  info_.nexthop = newNexthop;
  info_.distance = newDistance;
  info_.reportDistance = newDistance;
  info_.feasibleDistance = newDistance;
  // send out UPDATES if report-distance changed
  if (not sameRd) {
    floodUpdates(msgsToSend);
  }
}

bool
Dual::diffusingComputation(
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  // maintain current nexthop, update other fields
  auto ld = localDistances_[*info_.nexthop];
  auto rd = info_.neighborInfos[*info_.nexthop].reportDistance;
  int64_t newDistance = addDistances(ld, rd);
  info_.distance = newDistance;
  info_.reportDistance = newDistance;
  info_.feasibleDistance = newDistance;

  // send out diffusing queries
  bool success = false;

  thrift::DualMessage msg;
  msg.dstId = rootId;
  msg.distance = info_.reportDistance;
  msg.type = thrift::DualMessageType::QUERY;

  for (const auto& kv : localDistances_) {
    const auto& neighbor = kv.first;
    if (kv.second == std::numeric_limits<int64_t>::max()) {
      // skip down neighbor
      continue;
    }

    msgsToSend[neighbor].messages.emplace_back(msg);
    counters_[neighbor].querySent++;
    info_.neighborInfos[neighbor].expectReply = true;
    success = true;
  }
  return success;
}

void
Dual::tryLocalOrDiffusing(
    const DualEvent& event,
    bool needReply,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  auto affected = routeAffected();
  if (not affected) {
    if (needReply) {
      sendReply(msgsToSend);
    }
    return;
  }

  std::string newNexthop;
  int64_t newDistance;
  bool fc = meetFeasibleCondition(newNexthop, newDistance);
  if (not info_.nexthop.hasValue()) {
    CHECK_EQ(fc, true) << "my nexthop was invalid, must meet FC";
  }
  if (fc) {
    // meet FC, perform local computation
    localComputation(newNexthop, newDistance, msgsToSend);
    if (needReply) {
      sendReply(msgsToSend);
    }
  } else {
    // not meet FC, perform diffusing computation
    if (needReply and event != DualEvent::QUERY_FROM_SUCCESSOR) {
      // if received query from neighbor other than current next-hop,
      // send reply back before starting diffusing
      sendReply(msgsToSend);
    }
    VLOG(2) << rootId << "::" << nodeId << ": start diffusing";
    bool success = diffusingComputation(msgsToSend);
    if (success) {
      info_.sm.processEvent(event, false);
    } else {
      // can't send even one query out -> my current successor is down
      info_.nexthop = folly::none;
    }
  }
}

std::string
Dual::getStatusString() const noexcept {
  std::vector<std::string> counterStrs;
  for (const auto& kv : counters_) {
    const auto& neighbor = kv.first;
    const auto& counters = kv.second;
    counterStrs.emplace_back(folly::sformat(
        "{}: Q ({}, {}), R ({}, {}), U ({}, {})",
        neighbor,
        counters.querySent,
        counters.queryRecv,
        counters.replySent,
        counters.replyRecv,
        counters.updateSent,
        counters.updateRecv));
  }
  return folly::sformat(
      "{}::{}: {}\n{}",
      rootId,
      nodeId,
      info_.toString(),
      folly::join("\n", counterStrs));
}

bool
Dual::neighborUp(const std::string& neighbor) {
  if (localDistances_.count(neighbor) == 0) {
    return false;
  }
  return localDistances_.at(neighbor) != std::numeric_limits<int64_t>::max();
}

const Dual::RouteInfo&
Dual::getInfo() const noexcept {
  return info_;
}

int64_t
Dual::addDistances(int64_t d1, int64_t d2) {
  if (d1 == std::numeric_limits<int64_t>::max() or
      d2 == std::numeric_limits<int64_t>::max()) {
    return std::numeric_limits<int64_t>::max();
  }
  return d1 + d2;
}

void
Dual::peerUp(
    const std::string& neighbor,
    int64_t cost,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  LOG(INFO) << rootId << "::" << nodeId << ": LINK UP event from (" << neighbor
            << ", " << cost << ")";

  // update local-distance
  localDistances_[neighbor] = cost;
  info_.neighborInfos.emplace(neighbor, NeighborInfo());

  if (info_.sm.state == DualState::PASSIVE) {
    // passive
    tryLocalOrDiffusing(DualEvent::OTHERS, false, msgsToSend);
  } else {
    // active
    if (info_.neighborInfos[neighbor].expectReply) {
      // I expected a reply from this neighbor before and it just came up
      // this is equivlent to receiving a reply

      thrift::DualMessage msg;
      msg.dstId = rootId;
      msg.distance = info_.reportDistance;
      msg.type = thrift::DualMessageType::REPLY;
      processReply(neighbor, msg, msgsToSend);
    }
  }

  // send neighbor all route-table entries whose report-distance is valid
  // TODO: here we might already send neighbor a update from tryLocalOrDiffusing
  // (#updates sent can be optimized)

  thrift::DualMessage msg;
  msg.dstId = rootId;
  msg.distance = info_.reportDistance;
  msg.type = thrift::DualMessageType::UPDATE;
  msgsToSend[neighbor].messages.emplace_back(std::move(msg));
  counters_[neighbor].updateSent++;

  if (info_.neighborInfos[neighbor].needToReply) {
    info_.neighborInfos.at(neighbor).needToReply = false;

    thrift::DualMessage reply;
    reply.dstId = rootId;
    reply.distance = info_.reportDistance;
    reply.type = thrift::DualMessageType::REPLY;
    msgsToSend[neighbor].messages.emplace_back(std::move(reply));
    counters_[neighbor].replySent++;
  }
}

void
Dual::peerDown(
    const std::string& neighbor,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  LOG(INFO) << rootId << "::" << nodeId << ": LINK DOWN event from "
            << neighbor;

  // update local-distance and report-distance
  localDistances_[neighbor] = std::numeric_limits<int64_t>::max();
  info_.neighborInfos[neighbor].reportDistance =
      std::numeric_limits<int64_t>::max();
  DualEvent event = DualEvent::INCREASE_D;

  if (info_.sm.state == DualState::PASSIVE) {
    // passive
    tryLocalOrDiffusing(event, false, msgsToSend);
  } else {
    // active
    info_.sm.processEvent(event);
    if (info_.neighborInfos[neighbor].expectReply) {
      // expecting a reply from this neighbor, but it goes down
      // equivlent to receing a reply from this guy with max-distance.

      thrift::DualMessage msg;
      msg.dstId = rootId;
      msg.distance = std::numeric_limits<int64_t>::max();
      msg.type = thrift::DualMessageType::REPLY;
      processReply(neighbor, msg, msgsToSend);
    }
  }
}

void
Dual::peerCostChange(
    const std::string& neighbor,
    int64_t cost,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  LOG(INFO) << rootId << "::" << nodeId << ": LINK COST event from ("
            << neighbor << ", " << cost << ")";
  DualEvent event = cost > localDistances_[neighbor] ? DualEvent::INCREASE_D
                                                     : DualEvent::OTHERS;
  // update local-distance
  localDistances_[neighbor] = cost;

  if (info_.sm.state == DualState::PASSIVE) {
    // passive
    tryLocalOrDiffusing(event, false, msgsToSend);
  } else {
    // active
    // only update d while leaving rd, fd as-is
    if (info_.nexthop.hasValue() and *info_.nexthop == neighbor) {
      info_.distance = addDistances(
          cost, info_.neighborInfos[*info_.nexthop].reportDistance);
    }
    info_.sm.processEvent(event);
  }
}

void
Dual::processUpdate(
    const std::string& neighbor,
    const thrift::DualMessage& update,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  CHECK(update.type == thrift::DualMessageType::UPDATE);
  CHECK_EQ(update.dstId, rootId) << "received update dst-id: " << update.dstId
                                 << " != my-root-id: " << rootId;

  const auto& rd = update.distance;
  VLOG(2) << rootId << "::" << nodeId << ": received UPDATE from (" << neighbor
          << ", " << rd << ")";
  counters_[neighbor].updateRecv++;

  // update report-distance
  info_.neighborInfos[neighbor].reportDistance = rd;

  if (localDistances_.count(neighbor) == 0) {
    // received UPDATE before having local info_ (LINK-UP), done here
    return;
  }

  if (info_.sm.state == DualState::PASSIVE) {
    // passive
    tryLocalOrDiffusing(DualEvent::OTHERS, false, msgsToSend);
  } else {
    // active
    // only update d while leaving rd, fd as-is
    if (info_.nexthop.hasValue() and *info_.nexthop == neighbor) {
      info_.distance = addDistances(localDistances_[*info_.nexthop], rd);
    }
    info_.sm.processEvent(DualEvent::OTHERS);
  }
}

void
Dual::sendReply(
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  CHECK_GT(info_.cornet.size(), 0) << "send reply called on empty cornet";

  std::string dstNode = info_.cornet.top();
  info_.cornet.pop();

  if (not neighborUp(dstNode)) {
    // neighbor was expecting a reply from me, but link is down on my end
    // two cases:
    // 1. link was up on both end, and now it's down: we can wait for neighbor
    //    to receive a neighbor-down event (as-if neighbor received a reply)
    // 2. link is up on the other end, I received a query, but I haven't
    //    received a neighbor-up event yet. set pending-reply = true so when
    //    link is up on my end, I can send out reply.
    info_.neighborInfos[dstNode].needToReply = true;
    return;
  }

  thrift::DualMessage msg;
  msg.dstId = rootId;
  msg.distance = info_.reportDistance;
  msg.type = thrift::DualMessageType::REPLY;

  msgsToSend[dstNode].messages.emplace_back(std::move(msg));
  counters_[dstNode].replySent++;
}

void
Dual::processQuery(
    const std::string& neighbor,
    const thrift::DualMessage& query,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  CHECK(query.type == thrift::DualMessageType::QUERY);
  CHECK_EQ(query.dstId, rootId) << "received query dst-id: " << query.dstId
                                << " != my-root-id: " << rootId;

  const auto& rd = query.distance;
  VLOG(2) << rootId << "::" << nodeId << ": received QUERY from (" << neighbor
          << ", " << rd << ")";
  counters_[neighbor].queryRecv++;

  // update report-distance
  info_.neighborInfos[neighbor].reportDistance = rd;
  info_.cornet.emplace(neighbor);
  DualEvent event = DualEvent::OTHERS;
  if (info_.nexthop.hasValue() and *info_.nexthop == neighbor) {
    event = DualEvent::QUERY_FROM_SUCCESSOR;
  }

  if (info_.sm.state == DualState::PASSIVE) {
    // passive
    tryLocalOrDiffusing(event, true /* need reply */, msgsToSend);
  } else {
    // active
    if (info_.nexthop.hasValue() and *info_.nexthop == neighbor) {
      info_.distance = addDistances(
          localDistances_[*info_.nexthop],
          info_.neighborInfos[*info_.nexthop].reportDistance);
    }
    info_.sm.processEvent(event);
    sendReply(msgsToSend);
  }
}

void
Dual::processReply(
    const std::string& neighbor,
    const thrift::DualMessage& reply,
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  CHECK(reply.type == thrift::DualMessageType::REPLY);
  CHECK_EQ(reply.dstId, rootId) << "received reply dst-id: " << reply.dstId
                                << " != my-root-id: " << rootId;

  const auto& reportDistance = reply.distance;
  VLOG(2) << rootId << "::" << nodeId << ": received REPLY from (" << neighbor
          << ", " << reportDistance << ")";
  counters_[neighbor].replyRecv++;

  if (not info_.neighborInfos[neighbor].expectReply) {
    // received a reply when I don't expect to receive a reply from it
    // this is OK, this can happen when I detect link-down event before I
    // receive the reply, just ignore it.
    VLOG(2) << rootId << "::" << nodeId << " recv REPLY from " << neighbor
            << " while I dont expect a reply, ignore it";
    return;
  }

  // active
  // update report-distance and expect-reply flag
  info_.neighborInfos[neighbor].reportDistance = reportDistance;
  info_.neighborInfos[neighbor].expectReply = false;

  bool lastReply = true;
  for (const auto& kv : info_.neighborInfos) {
    if (kv.second.expectReply) {
      lastReply = false;
      break;
    }
  }
  if (not lastReply) {
    return;
  }

  // step1. all my dependent nodes have either modified their routes as a
  // result of the distance reported by me OR stopped being my dependent
  // Therefore, I'm free to pick the optimal solution
  info_.sm.processEvent(DualEvent::LAST_REPLY, true);

  int64_t d;
  int64_t dmin = std::numeric_limits<int64_t>::max();
  folly::Optional<std::string> newNh = folly::none;
  for (const auto& kv : localDistances_) {
    const auto& nb = kv.first;
    const auto& ld = kv.second;
    const auto& rd = info_.neighborInfos[nb].reportDistance;
    d = addDistances(ld, rd);
    if (d < dmin) {
      dmin = d;
      newNh = nb;
    }
  }
  bool sameRd = dmin == info_.reportDistance;
  info_.distance = dmin;
  info_.reportDistance = dmin;
  info_.feasibleDistance = dmin;
  info_.nexthop = newNh;
  if (not sameRd) {
    floodUpdates(msgsToSend);
  }

  // step2. check if I have pending reply to send out
  if (info_.cornet.size() > 0) {
    CHECK_EQ(info_.cornet.size(), 1)
        << nodeId << " one diffusing per destination, but my cornet has size "
        << info_.cornet.size();
    sendReply(msgsToSend);
  }
}

// class DualNode methods

DualNode::DualNode(const std::string& nodeId, bool isRoot)
    : nodeId(nodeId), isRoot(isRoot) {
  if (isRoot) {
    duals_.emplace(nodeId, Dual(nodeId, nodeId, localDistances_));
  }
}

void
DualNode::peerUp(const std::string& neighbor, int64_t cost) {
  // update local-distance
  localDistances_[neighbor] = cost;

  std::unordered_map<std::string, thrift::DualMessages> msgsToSend;

  for (auto& kv : duals_) {
    kv.second.peerUp(neighbor, cost, msgsToSend);
  }

  sendAllDualMessages(msgsToSend);
}

void
DualNode::peerDown(const std::string& neighbor) {
  // update local-distance
  localDistances_[neighbor] = std::numeric_limits<int64_t>::max();

  std::unordered_map<std::string, thrift::DualMessages> msgsToSend;

  for (auto& kv : duals_) {
    kv.second.peerDown(neighbor, msgsToSend);
  }

  sendAllDualMessages(msgsToSend);
}

void
DualNode::peerCostChange(const std::string& neighbor, int64_t cost) {
  // update local-distance
  localDistances_[neighbor] = cost;

  std::unordered_map<std::string, thrift::DualMessages> msgsToSend;

  for (auto& kv : duals_) {
    kv.second.peerCostChange(neighbor, cost, msgsToSend);
  }

  sendAllDualMessages(msgsToSend);
}

void
DualNode::processDualMessages(const thrift::DualMessages& messages) {
  std::unordered_map<std::string, thrift::DualMessages> msgsToSend;
  const auto& neighbor = messages.srcId;

  for (const auto& msg : messages.messages) {
    const auto& rootId = msg.dstId;
    // emplace a new Dual for this rootId if not exist
    duals_.emplace(rootId, Dual(nodeId, rootId, localDistances_));
    auto& dual = duals_.at(rootId);
    switch (msg.type) {
    case thrift::DualMessageType::UPDATE: {
      dual.processUpdate(neighbor, msg, msgsToSend);
      break;
    }
    case thrift::DualMessageType::QUERY: {
      dual.processQuery(neighbor, msg, msgsToSend);
      break;
    }
    case thrift::DualMessageType::REPLY: {
      dual.processReply(neighbor, msg, msgsToSend);
      break;
    }
    default: {
      LOG(ERROR) << "unknown dual message type";
      break;
    }
    }
  }

  sendAllDualMessages(msgsToSend);
}

folly::Optional<Dual::RouteInfo>
DualNode::getInfo(const std::string& rootId) const noexcept {
  if (duals_.count(rootId) == 0) {
    return folly::none;
  }
  return duals_.at(rootId).getInfo();
}

std::unordered_map<std::string, Dual::RouteInfo>
DualNode::getInfos() const noexcept {
  std::unordered_map<std::string, Dual::RouteInfo> infos;
  for (const auto& kv : duals_) {
    infos.emplace(kv.first, kv.second.getInfo());
  }
  return infos;
}

std::string
DualNode::getStatusString(const std::string& rootId) const noexcept {
  if (duals_.count(rootId) == 0) {
    return folly::sformat(
        "{}: route info for root {} not exist", nodeId, rootId);
  }
  return duals_.at(rootId).getStatusString();
}

std::unordered_map<std::string, std::string>
DualNode::getStatusStrings() const noexcept {
  std::unordered_map<std::string, std::string> allStatus;
  for (const auto& kv : duals_) {
    allStatus.emplace(kv.first, kv.second.getStatusString());
  }
  return allStatus;
}

bool
DualNode::neighborUp(const std::string& neighbor) {
  if (localDistances_.count(neighbor) == 0) {
    return false;
  }
  return localDistances_.at(neighbor) != std::numeric_limits<int64_t>::max();
}

void
DualNode::sendAllDualMessages(
    std::unordered_map<std::string, thrift::DualMessages>& msgsToSend) {
  for (auto& kv : msgsToSend) {
    // set srcId = myNodeId
    kv.second.srcId = nodeId;
    if (not sendDualMessages(kv.first, kv.second)) {
      LOG(ERROR) << "failed to send dual messages to " << kv.first;
    }
  }
}

} // namespace openr
