/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdexcept>

#include <folly/Format.h>

namespace openr {

class NetlinkException : public std::runtime_error {
 public:
  explicit NetlinkException(const std::string& exception)
      : std::runtime_error(
            folly::sformat("Netlink exception: {} ", exception)) {}
};
} // namespace openr
