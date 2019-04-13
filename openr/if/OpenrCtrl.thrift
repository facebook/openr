/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.OpenrCtrl

include "common/fb303/if/fb303.thrift"

enum OpenrModuleType {
  DECISION = 1,
  FIB = 2,
  HEALTH_CHECKER = 3,
  KVSTORE = 4,
  LINK_MONITOR = 5,
  PREFIX_ALLOCATOR = 6,
  PREFIX_MANAGER = 7,
  SPARK = 8,
}

exception OpenrError {
  1: string message
} ( message = "message" )

service OpenrCtrl extends fb303.FacebookService {
  binary command(1: OpenrModuleType module, 2: binary request)
    throws (1: OpenrError error)

  bool hasModule(1: OpenrModuleType module)
    throws (1: OpenrError error)
}
