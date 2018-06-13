/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

include "Platform.thrift"
include "IpPrefix.thrift"

namespace cpp2 openr.thrift
namespace py openr.LinuxPlatform

/**
 * Platform.fibService provides common functionality all Platforms must
 * must implement.
 *
 * Here we extend the service here to provide Linux Fib specific functionality
 * In this case it is to export the kernel routing table Other Fib agents can
 * export things like Hardware state, stats, counters, etc.
 */
service LinuxFibService extends Platform.FibService {

  list<IpPrefix.UnicastRoute> getKernelRouteTable() throws
      (1: Platform.PlatformError error)
}
