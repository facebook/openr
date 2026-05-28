/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <openr/tests/scale/Session.h>
#include <openr/tests/scale/tests/SessionTestUtil.h>

namespace openr {

TEST(SessionTest, ConstructorPopulatesTopology) {
  auto cfg = test::MakeTestConfig();
  Session session(cfg, /*basePortOverride=*/0);
  // start() not called; verify the ctor populated the topology and
  // listNodesUnlocked() returns a non-empty set of node names.
  auto names = session.listNodesUnlocked();
  EXPECT_FALSE(names.empty());
}

} // namespace openr
