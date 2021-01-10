/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <limits>
#include <stack>
#include <unordered_map>

#include <folly/Format.h>

#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

/**
 * DUAL states
 * PASSIVE: dual information is converged and ready to be used
 * ACTIVE[0-3]: dual computation is in process (waiting for dependents to
 *              coverge), information is NOT ready yet
 * there is no meaningful names for each different ACTIVE states, it's mainly
 * used for dual internally to handle things slightly differently. User should
 * only care about if dual is PASSIVE(ready) or ACTIVE(not ready).
 * details can be found: https://www.cs.cornell.edu/people/egs/615/lunes93.pdf
 */
enum class DualState {
  ACTIVE0,
  ACTIVE1,
  ACTIVE2,
  ACTIVE3,
  PASSIVE,
};

/**
 * DUAL input event that may change DUAL state
 */
enum class DualEvent {
  QUERY_FROM_SUCCESSOR, // receieved query from current successor
  LAST_REPLY, // received last diffusing reply
  INCREASE_D, // experienced distance increase
  OTHERS, // other event
};

/**
 * DUAL state machine
 */
struct DualStateMachine {
  DualState state{DualState::PASSIVE};
  // take input event, switch state if need
  // event: input event
  // fc: meet feasible condition or not
  void processEvent(DualEvent event, bool fc = true);
};

/**
 * DUAL (Diffusing Update Algorithm) Node
 * details refer to: https://www.cs.cornell.edu/people/egs/615/lunes93.pdf
 * This Module using DUAL to calculate shortest route towards destination root
 * This module is used internally to support multi-root, Users should use
 * class Dual directly.
 */
class Dual {
 public:
  // constructor
  // takes nodeId, rootId, current local-distances
  Dual(
      const std::string& nodeId,
      const std::string& rootId,
      const std::unordered_map<std::string, int64_t>& localDistance,
      std::function<void(
          const std::optional<std::string>& oldNh,
          const std::optional<std::string>& newNh)> nexthopChangeCb);

  // peer up event
  // input: (neighbor-id, link-metric)
  // output: map<neighbor-id: dual-messages-to-send>
  void peerUp(
      const std::string& neighbor,
      int64_t cost,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // peer down event
  // input: (neighbor-id)
  // output: map<neighbor-id: dual-messages-to-send>
  void peerDown(
      const std::string& neighbor,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // peer cost change event
  // input: (neighbor-id, new-link-metric)
  // output: map<neighbor-id: dual-messages-to-send>
  void peerCostChange(
      const std::string& neighbor,
      int64_t cost,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // process a DUAL update message
  // input: (neighbor-id, a update dual-message)
  // output: map<neighbor-id: dual-messages-to-send>
  void processUpdate(
      const std::string& neighbor,
      const thrift::DualMessage& update,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // process a DUAL query message
  // input: (neighbor-id, a query dual-message)
  // output: map<neighbor-id: dual-messages-to-send>
  void processQuery(
      const std::string& neighbor,
      const thrift::DualMessage& query,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // process a DUAL reply message
  // input: (neighbor-id, a reply dual-message)
  // output: map<neighbor-id: dual-messages-to-send>
  void processReply(
      const std::string& neighbor,
      const thrift::DualMessage& reply,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // Neighbor information per destination
  struct NeighborInfo {
    // neighbor reported distance towards destination
    int64_t reportDistance{std::numeric_limits<int64_t>::max()};
    // diffusing: expect receiving a reply from this neighbor or not
    bool expectReply{false};
    // diffusing: do I need to send a reply back to neighbor
    bool needToReply{false};
  };

  // route information per destination
  struct RouteInfo {
    // my current distance towards destination
    int64_t distance{std::numeric_limits<int64_t>::max()};
    // distance that will be used to report to my neighbors
    // this can be different from distance in case where node gets into
    // active state, distance will be updated in realtime while report-distance
    // and feasible-distance will stay same until it gets back to PASSIVE state
    int64_t reportDistance{std::numeric_limits<int64_t>::max()};
    // distance used to evaluate whether a feasible condition is met or not
    int64_t feasibleDistance{std::numeric_limits<int64_t>::max()};
    // my current nexthop towards destination, none if invalid
    std::optional<std::string> nexthop{std::nullopt};
    // state machine
    DualStateMachine sm;
    // neighbor exchanged information <report-distance, expect-reply-flag>
    std::unordered_map<std::string, NeighborInfo> neighborInfos;
    // diffusing: track received query
    std::stack<std::string> cornet{};

    // dump route info into human-friendly string mainly for logging or
    // debugging
    std::string
    toString() const {
      std::string state;
      switch (sm.state) {
      case DualState::ACTIVE0: {
        state = "ACTIVE0";
        break;
      }
      case DualState::ACTIVE1: {
        state = "ACTIVE1";
        break;
      }
      case DualState::ACTIVE2: {
        state = "ACTIVE2";
        break;
      }
      case DualState::ACTIVE3: {
        state = "ACTIVE3";
        break;
      }
      case DualState::PASSIVE: {
        state = "PASSIVE";
        break;
      }
      default: {
        state = "UNKNOWN";
        break;
      }
      }
      return folly::sformat(
          "[{}] {} ({}, {}, {})",
          state,
          nexthop.value_or("None"),
          distance,
          reportDistance,
          feasibleDistance);
    }
  };

  // get current route-info
  const RouteInfo& getInfo() const noexcept;

  // check if have a valid route towards root or not
  bool hasValidRoute() const noexcept;

  // get status string (includes route-info and dual-counters)
  std::string getStatusString() const noexcept;

  // get map<root-id: DualPerRootCounters>
  std::map<std::string, thrift::DualPerRootCounters> getCounters()
      const noexcept;

  // add a spt child
  void addChild(const std::string& child) noexcept;

  // remove a spt child
  void removeChild(const std::string& child) noexcept;

  // get current spt children
  std::unordered_set<std::string> children() const noexcept;

  // get current spt peers (nexthop + children)
  // return empty-set if dual has no valid route
  std::unordered_set<std::string> sptPeers() const noexcept;

  // my node id
  const std::string nodeId;

  // root id
  const std::string rootId;

 private:
  // get minimum distance towards root
  int64_t getMinDistance();

  // check if my route-to-root is affected
  bool routeAffected();

  // check if meet the feasible condition or not according to SNC (source node
  // condition)
  // if we can find a neighbor whose report-distance < my-feasible-distance
  // AND local-distance + report-distance == current minimum-distance
  // return true, otherwise return false
  bool meetFeasibleCondition(std::string& nexthop, int64_t& distance);

  // flood updates to all my neighbor
  void floodUpdates(
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // perform a local computation
  void localComputation(
      const std::string& newNexthop,
      int64_t newDistance,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // start diffuing computation (when not meet feasible condition)
  bool diffusingComputation(
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // perform local or diffusing computation depends on if FC is met
  // if needReply: send reply back
  void tryLocalOrDiffusing(
      const DualEvent& event,
      bool needReply,
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // helper to add two distances
  static int64_t addDistances(int64_t d1, int64_t d2);

  // send a reply back
  void sendReply(
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // check if a neighbor is up or not
  bool neighborUp(const std::string& neighbor);

  // clear counters to zero for a given neighbor
  void clearCounters(const std::string& neighbor) noexcept;

  // route-info towards root
  RouteInfo info_;

  // local neighbor distances
  std::unordered_map<std::string, int64_t> localDistances_;

  // dual messages counters map<neighbor: dual-counters>
  std::map<std::string, thrift::DualPerRootCounters> counters_;

  // callback when nexthop changed
  const std::function<void(
      const std::optional<std::string>& oldNh,
      const std::optional<std::string>& newNh)>
      nexthopCb_{nullptr};

  // spt children
  std::unordered_set<std::string> children_;
};

/**
 * DualNode takes a node-id, and a isRoot flag
 * DualNode will discover all roots on the fly
 * User need to hook up and feed following input events to DualNode properly
 * - peerUp()
 * - peerDown()
 * - peerCostChange()
 * - processDualMessages()
 * and use following getter() to retrieve route-info or status-string
 * - getInfo(s)
 * - getStatusString(s)
 *
 * Example usage:
 * auto node1 = DualNode("node1", true);
 * auto node2 = DualNode("node2", false);
 * node1.peerUp("node2", 12);
 * node2.peerUp("node1", 12);
 * auto infos = node1.getInfos();
 */
class DualNode {
 public:
  // constructor takes nodeId, and a indicator if I'm the root or not
  explicit DualNode(const std::string& nodeId, bool isRoot = false);

  virtual ~DualNode() = default;

  // subclass needs to implement this method to perform actual I/O operation
  // return true on success, otherwise false
  virtual bool sendDualMessages(
      const std::string& neighbor,
      const thrift::DualMessages& msgs) noexcept = 0;

  // subclass needs to override this api to perform actions when nexthop changes
  // for a given root-id
  virtual void processNexthopChange(
      const std::string& rootId,
      const std::optional<std::string>& oldNh,
      const std::optional<std::string>& newNh) noexcept = 0;

  // peer up from neighbor at link-metric cost
  void peerUp(const std::string& neighbor, int64_t cost);

  // peer down from neighbor
  void peerDown(const std::string& neighbor);

  // peer cost change from neighbor
  void peerCostChange(const std::string& neighbor, int64_t cost);

  // process dual messages
  void processDualMessages(const thrift::DualMessages& messages);

  // check if a given root-id is discovered or not
  bool hasDual(const std::string& rootId);

  // get reference to dual for a given root-id
  Dual& getDual(const std::string& rootId);

  // get all discovered duals reference as map<root-id: Dual>
  std::map<std::string, Dual>& getDuals();

  // pick smallest root-id who has a valid-route
  // return none if no ready SPT found
  std::optional<std::string> getSptRootId() const noexcept;

  // get SPT-peers for a given root-id
  // return empty-set if dual for root-id is not ready
  std::unordered_set<std::string> getSptPeers(
      const std::optional<std::string>& rootId) const noexcept;

  // get route-info for a given root-id
  // return none if root-id is not discoveried yet
  std::optional<Dual::RouteInfo> getInfo(
      const std::string& rootId) const noexcept;

  // get all route-infos for all discovered roots
  std::unordered_map<std::string, Dual::RouteInfo> getInfos() const noexcept;

  // get status as string for a given root-id
  std::string getStatusString(const std::string& rootId) const noexcept;

  // get status for all discovered roots
  // return pair<this-node-level-status, map<root-id: root-level-status>>
  std::pair<std::string, std::unordered_map<std::string, std::string>>
  getStatusStrings() const noexcept;

  // check if a neighbor is up or not
  bool neighborUp(const std::string& neighbor) const noexcept;

  // get dual related counters
  thrift::DualCounters getCounters() const noexcept;

  // myRootId
  const std::string nodeId;

  // I'm a root or not
  const bool isRoot{false};

 private:
  // send out dual messages for a given <neighbor: dual-messages>
  void sendAllDualMessages(
      std::unordered_map<std::string, thrift::DualMessages>& msgsToSend);

  // add Dual for a given root-id if not exist yet
  void addDual(const std::string& rootId);

  // clear counters to zero for a given neighbor
  void clearCounters(const std::string& neighbor) noexcept;

  // local distances map<neighbor: distance>
  std::unordered_map<std::string, int64_t> localDistances_;

  // map<root-id: Dual-object>
  std::map<std::string, Dual> duals_;

  // map<neighbor-id: counters>
  std::unordered_map<std::string, thrift::DualPerNeighborCounters> counters_;
};

} // namespace openr
