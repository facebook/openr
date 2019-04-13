/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <common/fb303/cpp/FacebookBase2.h>

namespace facebook {
namespace fb303 {

void
FacebookBase2::getCounters(std::map<std::string, int64_t>& /* _return */) {
  return;
}

void
FacebookBase2::getExportedValues(
    std::map<std::string, std::string>& /* _return */) {
  throw std::runtime_error("not implemented");
}

void
FacebookBase2::getRegexExportedValues(
    std::map<std::string, std::string>& /* _return */,
    std::unique_ptr<std::string> /* regex */) {
  throw std::runtime_error("not implemented");
}

void
FacebookBase2::getSelectedExportedValues(
    std::map<std::string, std::string>& /* _return */,
    std::unique_ptr<std::vector<std::string>> /* keys */) {
  throw std::runtime_error("not implemented");
}

void
FacebookBase2::getExportedValue(
    std::string& /* _return */, std::unique_ptr<std::string> /* key */) {
  throw std::runtime_error("not implemented");
}

} // namespace fb303
} // namespace facebook
