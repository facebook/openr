/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>

#include <gtest/gtest.h>
#include <openr/common/FileUtil.h>

using namespace openr;

TEST(FileUtilTest, ReadNonexistentFile) {
  std::string contents;
  EXPECT_FALSE(
      FileUtil::readFileToString("/nonexistent/path/file.txt", contents));
}

TEST(FileUtilTest, ReadExistingFile) {
  auto tmpPath = "/tmp/openr_fileutil_test_tmp";
  {
    std::ofstream ofs(tmpPath);
    ofs << "hello";
  }
  std::string contents;
  EXPECT_TRUE(FileUtil::readFileToString(tmpPath, contents));
  EXPECT_EQ(contents, "hello");
  std::remove(tmpPath);
}
