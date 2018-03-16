/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/BuildInfo.h>

namespace openr {

const char*
BuildInfo::getBuildUser() {
  return "";
}

const char*
BuildInfo::getBuildTime() {
  return "";
}

uint64_t
BuildInfo::getBuildTimeUnix() {
  return 0;
}

const char*
BuildInfo::getBuildHost() {
  return "";
}

const char*
BuildInfo::getBuildPath() {
  return "";
}

const char*
BuildInfo::getBuildRevision() {
  return "";
}

uint64_t
BuildInfo::getBuildRevisionCommitTimeUnix() {
  return 0;
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
  return "";
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
  return "";
}

const char*
BuildInfo::getBuildMode() {
  return "";
}

void
BuildInfo::log(std::ostream& /* os */) {
  // NOTE: Not logging anything. Replace this with your own implementation
  // if your usecase wants to log build information!
}

} // namespace openr
