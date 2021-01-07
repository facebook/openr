/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.PrefixManager
namespace py openr.PrefixManager
namespace py3 openr.thrift
namespace lua openr.PrefixManager

include "OpenrConfig.thrift"

// struct to represent originated prefix from PrefixManager's view
struct OriginatedPrefixEntry {
  1: OpenrConfig.OriginatedPrefix prefix
  2: list<string> supporting_prefixes = {}
  3: bool installed = 0
}
