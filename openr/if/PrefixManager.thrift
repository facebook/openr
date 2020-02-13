/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.PrefixManager
namespace py3 openr.thrift

include "Lsdb.thrift"
include "Network.thrift"

enum PrefixManagerCommand {
  ADD_PREFIXES = 1,
  WITHDRAW_PREFIXES = 2,
  WITHDRAW_PREFIXES_BY_TYPE = 3,
  SYNC_PREFIXES_BY_TYPE = 6,
}

struct PrefixManagerRequest {
  1: PrefixManagerCommand cmd
  // numbering on purpose
  3: list<Lsdb.PrefixEntry> prefixes
  // only applies to *_BY_TYPE commands
  4: Network.PrefixType type
}

struct PrefixManagerResponse {
  1: bool success
  2: string message
}
