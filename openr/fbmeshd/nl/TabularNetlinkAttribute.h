/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/attr.h> // @manual
#include <netlink/genl/genl.h> // @manual
#include <netlink/msg.h> // @manual

#include <array>
#include <cstddef>
#include <stdexcept>

#include <folly/Likely.h>

namespace openr {
namespace fbmeshd {

/**
 * A Netlink attribute with data fields
 *
 * This class should be used when the nlattr is a key-val map
 *
 * Before:
 * std::array<nlattr*, NL80211_BAND_ATTR_MAX + 1> tb_band;
 * nla_parse(
 *     tb_band.data(),
 *     NL80211_BAND_ATTR_MAX,
 *     (nlattr*)nla_data(nl_band),
 *     nla_len(nl_band),
 *     nullptr);
 * std::cout << tb_band[NL80211_BAND_ATTR_HT_MCS_SET];
 *
 * After:
 * TabularNetlinkAttribute<NL80211_BAND_ATTR_MAX> tb_band{nl_band};
 * std::cout << tb_band[NL80211_BAND_ATTR_HT_MCS_SET];
 */
template <size_t size>
class TabularNetlinkAttribute {
 private:
  explicit TabularNetlinkAttribute(
      nlattr* head, int len, const nla_policy* policy) {
    nla_parse(
        tb_attr_.data(), size, head, len, const_cast<nla_policy*>(policy));
  }

  explicit TabularNetlinkAttribute(nlattr* nlattrp, const nla_policy* policy)
      : TabularNetlinkAttribute(
            static_cast<nlattr*>(nla_data(nlattrp)), nla_len(nlattrp), policy) {
  }

 public:
  explicit TabularNetlinkAttribute(
      nlattr* head, int len, const std::array<nla_policy, size + 1>& policy)
      : TabularNetlinkAttribute(head, len, policy.data()) {}
  explicit TabularNetlinkAttribute(nlattr* head, int len)
      : TabularNetlinkAttribute(head, len, nullptr) {}

  explicit TabularNetlinkAttribute(
      nlattr* nlattrp, const std::array<nla_policy, size + 1>& policy)
      : TabularNetlinkAttribute(nlattrp, policy.data()) {}
  explicit TabularNetlinkAttribute(nlattr* nlattrp)
      : TabularNetlinkAttribute(nlattrp, nullptr) {}

  nlattr* operator[](size_t key) const {
    return tb_attr_.at(key);
  }

  nlattr*
  at(size_t key) const {
    const auto ret = operator[](key);
    if (FOLLY_UNLIKELY(ret == nullptr)) {
      throw std::out_of_range("netlink attribute doesn't exist");
    }
    return ret;
  }

 private:
  std::array<nlattr*, size + 1> tb_attr_;
};

} // namespace fbmeshd
} // namespace openr
