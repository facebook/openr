/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift

include "KvStore.thrift"
include "OpenrCtrl.thrift"

/**
 * Extends OpenrCtrl and implements stream APIs as streams are only
 * supported in C++
 */
service OpenrCtrlCpp extends OpenrCtrl.OpenrCtrl {
  /**
   * Subscribe KvStore updates
   */
  stream<KvStore.Publication> subscribeKvStore()
}
