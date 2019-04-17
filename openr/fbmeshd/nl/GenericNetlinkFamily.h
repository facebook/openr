/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/Optional.h>

namespace openr {
namespace fbmeshd {

/**
 * Holds a list of Generic Netlink Family IDs
 * The family ID gets resolved on the first invocation of operator int() on
 * any family name;
 */
class GenericNetlinkFamily {
 public:
  static const GenericNetlinkFamily& NL80211();
  static const GenericNetlinkFamily& NLCTRL();

  explicit GenericNetlinkFamily(std::string family);

  GenericNetlinkFamily(const GenericNetlinkFamily&) = delete;

  explicit operator int() const;

 private:
  std::string family_;
  mutable folly::Optional<int> familyId_;
};

} // namespace fbmeshd
} // namespace openr
