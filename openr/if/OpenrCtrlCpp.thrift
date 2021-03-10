/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py3 openr.thrift
namespace wiki Open_Routing.Thrift_APIs.OpenrCtrlCpp

include "openr/if/OpenrCtrl.thrift"
include "openr/if/Types.thrift"

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
  Types.Publication, stream<Types.Publication> subscribeAndGetKvStore();
  // DEPRECATED
  Types.Publication, stream<Types.Publication> subscribeAndGetKvStoreFiltered(
    1: Types.KeyDumpParams filter,
  );

  // Prefer this API, above do not specfy area to subcribe to, default
  // constructing filter will yield all kvstore keys. provide set of areas to
  // dump and subscribe to. Providing an empty set will subscribe on all areas
  list<Types.Publication>, stream<
    Types.Publication
  > subscribeAndGetAreaKvStores(
    1: Types.KeyDumpParams filter,
    2: set<string> selectAreas,
  );

  /**
   * Retrieve Fib snapshot and subscribe for subsequent updates.
   * No update between snapshot and fullstream will be lost,
   * though there may be some replicated entries in stream that
   * are also in snapshot.
   */

  Types.RouteDatabase, stream<Types.RouteDatabaseDelta> subscribeAndGetFib();
  OpenrCtrl.RouteDatabaseDetail, stream<
    OpenrCtrl.RouteDatabaseDeltaDetail
  > subscribeAndGetFibDetail();
}
