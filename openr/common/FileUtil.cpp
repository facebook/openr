/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/FileUtil.h"
#include <folly/logging/xlog.h>

namespace openr {

bool
FileUtil::readFileToString(const std::string& path, std::string& contents) {
  // realpath() expands all symbolic links and resolves references to
  // /./, /../ and extra '/' characters in the null-terminated string
  // named by path to produce a canonicalized absolute pathname.

  // This prevents directory traversal attacks, e.g. writing to
  // "../../../etc/passwd".
  std::array<char, PATH_MAX + 1> resolvedPath;
  realpath(path.c_str(), resolvedPath.data());
  XLOG(INFO) << "Read the file: " << resolvedPath.data();

  // read the file
  return folly::readFile(resolvedPath.data(), contents);
}

} // namespace openr
