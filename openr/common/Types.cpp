/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <folly/logging/xlog.h>

#include <openr/common/Types.h>
#include <re2/re2.h>

namespace openr {

RegexSet::RegexSet(std::vector<std::string> const& keyPrefixList) {
  if (keyPrefixList.empty()) {
    return;
  }
  re2::RE2::Options re2Options;
  re2Options.set_case_sensitive(true);
  regexSet_ =
      std::make_unique<re2::RE2::Set>(re2Options, re2::RE2::ANCHOR_START);
  std::string re2AddError{};

  for (auto const& keyPrefix : keyPrefixList) {
    if (regexSet_->Add(keyPrefix, &re2AddError) < 0) {
      throw RegexSetException(
          fmt::format(
              "Failed to add prefixes to RE2 set: '{}', error: '{}'",
              keyPrefix,
              re2AddError));
      return;
    }
  }
  if (!regexSet_->Compile()) {
    throw RegexSetException("Failed to compile re2 set");
  }
}

bool
RegexSet::match(std::string const& key) const {
  CHECK(regexSet_);
  std::vector<int> matches;
  return regexSet_->Match(key, &matches);
}

} // namespace openr
