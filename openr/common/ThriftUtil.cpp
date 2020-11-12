/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if __has_include("filesystem")
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#endif

#include "openr/common/Flags.h"
#include "openr/common/ThriftUtil.h"
#include "openr/common/Util.h"

namespace openr {

// Setup thrift server for TLS
void
setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    apache::thrift::SSLPolicy sslPolicy,
    std::string const& ticketSeedPath,
    std::shared_ptr<wangle::SSLContextConfig> sslContext) {
  thriftServer.setSSLPolicy(sslPolicy);
  // Allow non-secure clients on localhost (e.g. breeze / fbmeshd)
  thriftServer.setAllowPlaintextOnLoopback(true);
  thriftServer.setSSLConfig(sslContext);
  if (fs::exists(ticketSeedPath)) {
    thriftServer.watchTicketPathForChanges(ticketSeedPath, true);
  }
  return;
}

} // namespace openr
