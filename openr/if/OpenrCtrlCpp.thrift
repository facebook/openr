/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py3 openr.thrift

include "openr/if/KvStore.thrift"
include "openr/if/OpenrCtrl.thrift"

/**
 * Extends OpenrCtrl and implements stream APIs as streams are only
 * supported in C++
 */
service OpenrCtrlCpp extends OpenrCtrl.OpenrCtrl {
  /**
   * Retrieve KvStore snapshot and as well subscribe subsequent updates. This
   * is useful for mirroring copy of KvStore on remote node for monitoring or
   * control applications. No update between snapshot and full-stream is lost.
   *
   * There may be some replicated entries in stream that are also in snapshot.
   */
  KvStore.Publication, stream<KvStore.Publication> subscribeAndGetKvStore()

  KvStore.Publication, stream<KvStore.Publication>
    subscribeAndGetKvStoreFiltered(1: KvStore.KeyDumpParams filter)
}
