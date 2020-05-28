/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <boost/serialization/strong_typedef.hpp>

namespace openr {

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

// PlatformPublisher Urls (right now we use global for everything)
BOOST_STRONG_TYPEDEF(std::string, PlatformPublisherUrl);

} // namespace openr
