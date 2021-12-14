/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/FileUtil.h>

namespace openr {

class FileUtil {
 public:
  /**
   * Read entire file and save it to `contents` param.
   *
   * Returns: true on success or false on failure. In the latter case
   * errno will be set appropriately by the failing system primitive.
   */
  static bool readFileToString(
      const std::string& filePath, std::string& contents);

 private:
  FileUtil() {}
};

} // namespace openr
