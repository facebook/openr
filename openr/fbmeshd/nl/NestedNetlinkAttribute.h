/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/attr.h> // @manual

#include <iterator>

namespace openr {
namespace fbmeshd {

/**
 * A Netlink attribute that contains nested attributes
 *
 * This class should be used when you want to iterate over the nested attributes
 * for a nlattr, by means of an iterator.
 *
 * Before:
 * nlattr* nl_mode;
 * int rem_mode;
 * nla_for_each_nested(
 *         nl_mode, tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES], rem_mode) {
 *   ...
 * }
 *
 * After:
 * for (const auto& nl_mode :
 *         NestedNetlinkAttribute{tb_msg[NL80211_ATTR_SUPPORTED_IFTYPES]}) {
 *   ...
 * }
 */
class NestedNetlinkAttribute {
 public:
  /**
   * Iterator to access nested attributes of this nlattr.
   * The end iterator is represented by a nullptr nlattr* attribute.
   */
  struct iterator : std::iterator<std::forward_iterator_tag, nlattr*> {
    iterator() = default;
    explicit iterator(nlattr* nla)
        : pos{static_cast<nlattr*>(nla_data(nla))}, rem{nla_len(nla)} {}

    iterator&
    operator++() {
      pos = nla_next(pos, &rem);
      if (!nla_ok(pos, rem)) {
        pos = nullptr;
      }
      return *this;
    }

    iterator
    operator++(int) {
      iterator __tmp = *this;
      ++__tmp;
      return __tmp;
    }

    bool
    operator==(const iterator& other) const {
      return other.pos == pos;
    }

    bool
    operator!=(const iterator& other) const {
      return other.pos != pos;
    }

    nlattr* operator*() const {
      return pos;
    };

   private:
    nlattr* pos{nullptr};
    int rem;
  };

  NestedNetlinkAttribute(nlattr* nlattrp) : nlattrp_{nlattrp} {}

  iterator
  begin() {
    return iterator{nlattrp_};
  }

  iterator
  end() {
    return iterator{};
  }

 private:
  nlattr* nlattrp_;
};

} // namespace fbmeshd
} // namespace openr
