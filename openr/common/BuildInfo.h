/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <inttypes.h>
#include <iostream>

namespace openr {

/**
 * Placeholder class for encoding build information into binary!
 */
class BuildInfo {
 public:

  /**
   * Unix name of the user that built this binary
   */
  static const char* getBuildUser();

  /**
   * Timestamp of when the binary was built, as a human-readable string
   */
  static const char* getBuildTime();

  /**
   * Timestamp of when the binary was built, as seconds since the epoch.
   */
  static uint64_t getBuildTimeUnix();

  /**
   * Host on which the binary was built.
   */
  static const char* getBuildHost();

  /**
   * Path from which the binary was built.
   */
  static const char* getBuildPath();

  /**
   * The git revision of the fbcode repository that this binary was built from.
   */
  static const char* getBuildRevision();

  /**
   * Timestamp of when the build revision was committed, as seconds
   * since the epoch.
   */
  static uint64_t getBuildRevisionCommitTimeUnix();

  /**
   * The most recent revision from origin/master that this binary was built
   * from.
   */
  static const char* getBuildUpstreamRevision();

  /**
   * Timestamp of when the most recent revision from origin/master that this
   * binary was built from was committed, as seconds since the epoch.
   */
  static uint64_t getBuildUpstreamRevisionCommitTimeUnix();

  /**
   * Name of the FBPackage that this binary was built for.
   */
  static const char* getBuildPackageName();

  /**
   * Version of the FBPackage that this binary was built for.
   */
  static const char* getBuildPackageVersion();

  /**
   * Release of the FBPackage that this binary was built for.
   */
  static const char* getBuildPackageRelease();

  /**
   * The fbcode platform this binary was built for.
   */
  static const char* getBuildPlatform();

  /**
   * The fbcode rule this binary was built with.
   */
  static const char* getBuildRule();

  /**
   * The fbcode rule type this binary was built with
   */
  static const char* getBuildType();

  /**
  * The build tool that built this binary
  */
  static const char* getBuildTool();

  /**
   * The fbcode build mode this binary was built for.
   */
  static const char* getBuildMode();

  /**
   * Call with log(LOG(INFO)) to log to info log.
   */
  static void log(std::ostream& os);

 private:
  BuildInfo() {};
};

} // namespace openr
