/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/FileUtil.h"

namespace openr {

bool
FileUtil::readFileToString(const std::string& filePath, std::string& contents) {
  return folly::readFile(filePath.c_str(), contents);
}

} // namespace openr
