/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/tests/scale/if/gen-cpp2/ScaleTestServer_types.h>

namespace openr::test {

// Pure-topology config: no DUT connect, no fake kvstore, no spark — suitable
// for ctor-only / pure-logic tests.
thrift::ScaleTestConfig MakeTestConfig();

} // namespace openr::test
