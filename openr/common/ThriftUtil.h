/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <wangle/ssl/SSLContextConfig.h>

namespace openr {

// Setup thrift server for TLS
void setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext);

} // namespace openr
