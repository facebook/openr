/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp openr.thrift
namespace cpp2 openr.thrift
namespace py openr.Decision
namespace py3 openr.thrift
namespace lua openr.Decision

include "Lsdb.thrift"

typedef map<string, Lsdb.AdjacencyDatabase>
  (
    cpp.type =
    "std::unordered_map<std::string, openr::thrift::AdjacencyDatabase>"
  ) AdjDbs

typedef map<string, Lsdb.PrefixDatabase>
  (cpp.type = "std::unordered_map<std::string, openr::thrift::PrefixDatabase>")
  PrefixDbs

typedef map<string, Lsdb.PrefixEntry>
  (
    cpp.type =
    "std::unordered_map<std::string /* area */, openr::thrift::PrefixEntry>"
  )
  PrefixEntriesByAreaId

// prefix entries per prefix: by originator node and area
typedef map<string, PrefixEntriesByAreaId>
  (
    cpp.type =
    "std::unordered_map<std::string /* node */, std::unordered_map<std::string /* area */, openr::thrift::PrefixEntry>>"
  )
  PrefixEntries
