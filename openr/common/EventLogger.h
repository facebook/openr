/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Format.h>
#include <string>

namespace openr {

constexpr auto kOpenrEventPrefix("OPENR_EVENT");

struct EventTag {
  explicit EventTag(
      std::string prefix = kOpenrEventPrefix, std::string sub_type = "")
      : prefix_(std::move(prefix)), sub_type_(std::move(sub_type)){};

  friend std::ostream&
  operator<<(std::ostream& out, const EventTag& tag) {
    // Note: outputs a blank space at end
    if (!tag.sub_type_.empty()) {
      return (out << tag.prefix_ << "(" << tag.sub_type_ << "): ");
    } else {
      return (out << tag.prefix_ << ": ");
    }
  }

  const std::string
  str(void) const {
    // Note: outputs a blank space at end
    if (!sub_type_.empty()) {
      return folly::sformat("{}({}): ", prefix_, sub_type_);
    } else {
      return folly::sformat("{}: ", prefix_);
    }
  }

  const std::string prefix_;
  const std::string sub_type_;
};

} // namespace openr
