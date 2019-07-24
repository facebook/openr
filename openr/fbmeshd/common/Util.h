/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sstream>
#include <string>

#include <glog/logging.h>

#include <folly/Format.h>
#include <folly/MacAddress.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/Range.h>

namespace openr {
namespace fbmeshd {

// Convert a GFlags CSV string to a std::vector by applying a processing
// function for individual elements of the list
template <class T>
std::vector<T>
parseCsvFlag(
    const std::string& csvString,
    const std::function<T(const std::string&)>& processFunc) {
  VLOG(8) << folly::sformat("::{}()", __func__);

  if (csvString.empty()) {
    return {};
  }

  std::vector<T> result;
  std::stringstream ss{csvString};

  while (ss.good()) {
    std::string substr;
    getline(ss, substr, ',');
    result.push_back(processFunc(substr));
  }

  return result;
}

} // namespace fbmeshd
} // namespace openr
