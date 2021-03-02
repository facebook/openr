/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/BuildInfo.h>
#include <openr/common/CMakeBuildInfo.h>

#include <folly/Format.h>

namespace openr {

const char*
BuildInfo::getBuildUser() {
  return BuildInfo_kUser;
}

const char*
BuildInfo::getBuildTime() {
  return BuildInfo_kTime;
}

uint64_t
BuildInfo::getBuildTimeUnix() {
  return BuildInfo_kTimeUnix;
}

const char*
BuildInfo::getBuildHost() {
  return BuildInfo_kHost;
}

const char*
BuildInfo::getBuildPath() {
  return BuildInfo_kPath;
}

const char*
BuildInfo::getBuildRevision() {
  return BuildInfo_kRevision;
}

uint64_t
BuildInfo::getBuildRevisionCommitTimeUnix() {
  return BuildInfo_kRevisionCommitTimeUnix;
}

const char*
BuildInfo::getBuildUpstreamRevision() {
  return "";
}

uint64_t
BuildInfo::getBuildUpstreamRevisionCommitTimeUnix() {
  return 0;
}

const char*
BuildInfo::getBuildPackageName() {
  return "";
}

const char*
BuildInfo::getBuildPackageVersion() {
  return "";
}

const char*
BuildInfo::getBuildPackageRelease() {
  return "";
}

const char*
BuildInfo::getBuildPlatform() {
  return BuildInfo_kPlatform;
}

const char*
BuildInfo::getBuildRule() {
  return "";
}

const char*
BuildInfo::getBuildType() {
  return "";
}

const char*
BuildInfo::getBuildTool() {
  return BuildInfo_kBuildTool;
}

const char*
BuildInfo::getBuildMode() {
  return BuildInfo_kBuildMode;
}

void
BuildInfo::log(std::ostream& os) {
  os << folly::format(
      "\n  Built by: {}\n"
      "  Built on: {} ({})\n"
      "  Built at: {}\n"
      "  Build tool: {}\n"
      "  Build path: {}\n"
      "  Build Revision: {}\n"
      "  Build Platform: {} ({})",
      BuildInfo::getBuildUser(),
      BuildInfo::getBuildTime(),
      BuildInfo::getBuildTimeUnix(),
      BuildInfo::getBuildHost(),
      BuildInfo::getBuildTool(),
      BuildInfo::getBuildPath(),
      BuildInfo::getBuildRevision(),
      BuildInfo::getBuildPlatform(),
      BuildInfo::getBuildMode());
}

void
BuildInfo::exportBuildInfo() {
  return;
}

} // namespace openr
