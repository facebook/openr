/**
 * Copyright (c) 2014-present, Facebook, Inc.
 */

#include <openr/common/BuildInfo.h>

#include "common/base/BuildInfo.h"

namespace openr {

const char*
BuildInfo::getBuildUser() {
  return facebook::BuildInfo::getBuildUser();
}

const char*
BuildInfo::getBuildTime() {
  return facebook::BuildInfo::getBuildTime();
}

uint64_t
BuildInfo::getBuildTimeUnix() {
  return facebook::BuildInfo::getBuildTimeUnix();
}

const char*
BuildInfo::getBuildHost() {
  return facebook::BuildInfo::getBuildHost();
}

const char*
BuildInfo::getBuildPath() {
  return facebook::BuildInfo::getBuildPath();
}

const char*
BuildInfo::getBuildRevision() {
  return facebook::BuildInfo::getBuildRevision();
}

uint64_t
BuildInfo::getBuildRevisionCommitTimeUnix() {
  return facebook::BuildInfo::getBuildRevisionCommitTimeUnix();
}

const char*
BuildInfo::getBuildUpstreamRevision() {
  return facebook::BuildInfo::getBuildUpstreamRevision();
}

uint64_t
BuildInfo::getBuildUpstreamRevisionCommitTimeUnix() {
  return facebook::BuildInfo::getBuildUpstreamRevisionCommitTimeUnix();
}

const char*
BuildInfo::getBuildPackageName() {
  return facebook::BuildInfo::getBuildPackageName();
}

const char*
BuildInfo::getBuildPackageVersion() {
  return facebook::BuildInfo::getBuildPackageVersion();
}

const char*
BuildInfo::getBuildPackageRelease() {
  return facebook::BuildInfo::getBuildPackageRelease();
}

const char*
BuildInfo::getBuildPlatform() {
  return facebook::BuildInfo::getBuildPlatform();
}

const char*
BuildInfo::getBuildRule() {
  return facebook::BuildInfo::getBuildRule();
}

const char*
BuildInfo::getBuildType() {
  return facebook::BuildInfo::getBuildType();
}

const char*
BuildInfo::getBuildTool() {
  return facebook::BuildInfo::getBuildTool();
}

const char*
BuildInfo::getBuildMode() {
  return facebook::BuildInfo::getBuildMode();
}

void
BuildInfo::log(std::ostream& os) {
  facebook::BuildInfo::log(os);
}

} // namespace openr
