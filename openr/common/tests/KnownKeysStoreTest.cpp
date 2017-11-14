/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/KnownKeysStore.h>

#include <stdlib.h>
#include <unistd.h>

#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <gtest/gtest.h>

using namespace folly;
using namespace std;
using namespace openr;

namespace {
const char* kTempPublicKeyFileNameStr = "/tmp/pubKeyFile.XXXXXX";
}

TEST(KnownKeysStoreTest, SaveAndStoreKeys) {
  //
  // Prepare file name space
  //
  char tempPubKeyFileName[64];

  ::memset(tempPubKeyFileName, 0, sizeof(tempPubKeyFileName));

  ::memcpy(
      tempPubKeyFileName,
      kTempPublicKeyFileNameStr,
      strlen(kTempPublicKeyFileNameStr) + 1);

  //
  // Create temp files & install cleanup hooks
  //

  int fd;
  fd = mkstemp(tempPubKeyFileName);
  SCOPE_EXIT {
    unlink(tempPubKeyFileName);
    close(fd);
  };

  //
  // Init key store
  //

  KnownKeysStore store(tempPubKeyFileName);

  store.setKeyByName("peer1", "key1");
  store.setKeyByName("peer2", "key2");

  store.saveKeysToDisk();

  KnownKeysStore store2(tempPubKeyFileName);

  EXPECT_EQ("key1", store2.getKeyByName("peer1"));
  EXPECT_EQ("key2", store2.getKeyByName("peer2"));

  EXPECT_THROW(store.getKeyByName("peer3"), out_of_range);
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
