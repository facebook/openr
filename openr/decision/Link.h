/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/if/gen-cpp2/Network_types.h>
#include <openr/if/gen-cpp2/Types_types.h>

namespace openr {

using LinkStateMetric = uint64_t;

class Link {
 public:
  Link(
      const std::string& area,
      const std::string& nodeName1,
      const std::string& if1,
      const std::string& nodeName2,
      const std::string& if2,
      bool usable = true);

  Link(
      const std::string& area,
      const std::string& nodeName1,
      const openr::thrift::Adjacency& adj1,
      const std::string& nodeName2,
      const openr::thrift::Adjacency& adj2,
      bool usable = true);

 private:
  // link can only belongs to one specific area
  const std::string area_;

  // node names and link names from both ends
  const std::string n1_, n2_, if1_, if2_;

  // metric from both ends
  LinkStateMetric metric1_{1}, metric2_{1};

  // interface overload(Hard-drain) state
  bool overload1_{false}, overload2_{false};

  // whether link can be used due to device initilization state
  bool usable_{true};

  // adjacency label for segment routing use case
  int32_t adjLabel1_{0}, adjLabel2_{0};

  // weight represents a link's capacity (ex: 100Gbps)
  int64_t weight1_{}, weight2_{};

  // link addresses including v4 and v6
  thrift::BinaryAddress nhV41_, nhV42_, nhV61_, nhV62_;

  const std::pair<
      std::pair<std::string, std::string>,
      std::pair<std::string, std::string>>
      orderedNames_;

 public:
  const size_t hash{0};

  /*
   * [Accessor Method]
   */
  bool isUp() const;

  const std::string& getArea() const;

  const std::string& getOtherNodeName(const std::string& nodeName) const;

  const std::string& firstNodeName() const;

  const std::string& secondNodeName() const;

  const std::string& getIfaceFromNode(const std::string& nodeName) const;

  LinkStateMetric getMetricFromNode(const std::string& nodeName) const;

  LinkStateMetric getMaxMetric() const;

  int32_t getAdjLabelFromNode(const std::string& nodeName) const;

  int64_t getWeightFromNode(const std::string& nodeName) const;

  bool getOverloadFromNode(const std::string& nodeName) const;

  const thrift::BinaryAddress& getNhV4FromNode(
      const std::string& nodeName) const;

  const thrift::BinaryAddress& getNhV6FromNode(
      const std::string& nodeName) const;

  bool getUsability() const;

  /*
   * [Mutator Method]
   */
  void setNhV4FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV4);

  void setNhV6FromNode(
      const std::string& nodeName, const thrift::BinaryAddress& nhV6);

  bool setMetricFromNode(const std::string& nodeName, LinkStateMetric d);

  void setAdjLabelFromNode(const std::string& nodeName, int32_t adjLabel);

  void setWeightFromNode(const std::string& nodeName, int64_t weight);

  bool setOverloadFromNode(const std::string& nodeName, bool overload);

  bool setLinkUsability(const Link& newLink);

  bool operator<(const Link& other) const;

  bool operator==(const Link& other) const;

  std::string toString() const;

  std::string directionalToString(const std::string& fromNode) const;

  struct LinkPtrHash {
    size_t operator()(const std::shared_ptr<Link>& l) const;
  };

  struct LinkPtrLess {
    bool operator()(
        const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const;
  };

  struct LinkPtrEqual {
    bool operator()(
        const std::shared_ptr<Link>& lhs,
        const std::shared_ptr<Link>& rhs) const;
  };

  using LinkSet =
      folly::F14FastSet<std::shared_ptr<Link>, LinkPtrHash, LinkPtrEqual>;
}; // class Link

} // namespace openr

namespace std {

// needed for certain containers

template <>
struct hash<openr::Link> {
  size_t operator()(openr::Link const& link) const;
};

template <>
struct hash<openr::Link::LinkSet> {
  size_t operator()(openr::Link::LinkSet const& set) const;
};

template <>
struct equal_to<openr::Link::LinkSet> {
  bool operator()(
      openr::Link::LinkSet const& a, openr::Link::LinkSet const& b) const;
};

} // namespace std
