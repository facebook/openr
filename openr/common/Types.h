/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <utility>

#include <boost/serialization/strong_typedef.hpp>

#include <openr/if/gen-cpp2/Lsdb_types.h>

namespace openr {

//
// Aliases for data-structures
//
using NodeAndArea = std::pair<std::string, std::string>;
using PrefixEntries = std::unordered_map<NodeAndArea, thrift::PrefixEntry>;

// KvStore URLs
BOOST_STRONG_TYPEDEF(std::string, KvStoreGlobalCmdUrl);

// KvStore TCP ports
BOOST_STRONG_TYPEDEF(uint16_t, KvStoreCmdPort);

// OpenrCtrl Thrift port
BOOST_STRONG_TYPEDEF(uint16_t, OpenrCtrlThriftPort);

// Monitor URLs
BOOST_STRONG_TYPEDEF(std::string, MonitorSubmitUrl);
BOOST_STRONG_TYPEDEF(std::string, MonitorPubUrl);

// markers for some of KvStore keys
BOOST_STRONG_TYPEDEF(std::string, AdjacencyDbMarker);
BOOST_STRONG_TYPEDEF(std::string, PrefixDbMarker);
BOOST_STRONG_TYPEDEF(std::string, AllocPrefixMarker);

} // namespace openr
