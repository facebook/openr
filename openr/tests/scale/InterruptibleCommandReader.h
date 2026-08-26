/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <string>

namespace openr {

/*
 * Read newline-delimited commands until stdin closes or stopFd becomes
 * readable. The callback runs on the calling thread.
 */
void readCommandsUntilStopped(
    int inputFd, int stopFd, const std::function<void(std::string)>& onCommand);

} // namespace openr
