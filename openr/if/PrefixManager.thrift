/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.PrefixManager
namespace py3 openr.thrift
namespace lua openr.PrefixManager

include "Lsdb.thrift"
include "Network.thrift"

enum PrefixUpdateCommand {
  ADD_PREFIXES = 1,
  WITHDRAW_PREFIXES = 2,
  WITHDRAW_PREFIXES_BY_TYPE = 3,
  SYNC_PREFIXES_BY_TYPE = 6,
}

struct PrefixUpdateRequest {
  1: PrefixUpdateCommand cmd
  2: optional Network.PrefixType type
  3: list<Lsdb.PrefixEntry> prefixes
  // empty list = inject to all configured areas
  4: set<string> dstAreas = {} (cpp2.type = "std::unordered_set<std::string>")
}
