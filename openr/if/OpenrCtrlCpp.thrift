/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py3 openr.thrift

include "openr/if/Fib.thrift"
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
  // DEPRECATED
  KvStore.Publication, stream<KvStore.Publication> subscribeAndGetKvStore()
  // DEPRECATED
  KvStore.Publication, stream<KvStore.Publication>
    subscribeAndGetKvStoreFiltered(1: KvStore.KeyDumpParams filter)

  // Prefer this API, above do not specfy area to subcribe to, default
  // constructing filter will yield all kvstore keys. provide set of areas to
  // dump and subscribe to. Providing an empty set will subscribe on all areas
  list<KvStore.Publication>, stream<KvStore.Publication>
  subscribeAndGetAreaKvStores(
    1: KvStore.KeyDumpParams filter,
    2: set<string> selectAreas,
  )

  /**
   * Retrieve Fib snapshot and subscribe for subsequent updates.
   * No update between snapshot and fullstream will be lost,
   * though there may be some replicated entries in stream that
   * are also in snapshot.
   */

  Fib.RouteDatabase, stream<Fib.RouteDatabaseDelta> subscribeAndGetFib()

}
