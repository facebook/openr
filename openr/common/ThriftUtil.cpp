/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/ThriftUtil.h"
#include "openr/common/Flags.h"
#include "openr/common/Util.h"

namespace openr {

// Setup thrift server for TLS
void
setupThriftServerTls(
    apache::thrift::ThriftServer& thriftServer,
    std::string const& context,
    apache::thrift::SSLPolicy sslPolicy,
    folly::SSLContext::SSLVerifyPeerEnum clientVerification) {
  CHECK(fileExists(FLAGS_x509_ca_path));
  CHECK(fileExists(FLAGS_x509_cert_path));
  auto& keyPath = FLAGS_x509_key_path;
  if (!keyPath.empty()) {
    CHECK(fileExists(keyPath));
  } else {
    keyPath = FLAGS_x509_cert_path;
  }
  thriftServer.setSSLPolicy(sslPolicy);
  // Allow non-secure clients on localhost (e.g. breeze / fbmeshd)
  thriftServer.setAllowPlaintextOnLoopback(true);

  auto sslContext = std::make_shared<wangle::SSLContextConfig>();
  sslContext->setCertificate(FLAGS_x509_cert_path, keyPath, "");
  sslContext->clientCAFile = FLAGS_x509_ca_path;
  sslContext->sessionContext = context;
  sslContext->setNextProtocols(Constants::getNextProtocolsForThriftServers());
  sslContext->clientVerification = clientVerification;
  sslContext->eccCurveName = FLAGS_tls_ecc_curve_name;
  thriftServer.setSSLConfig(sslContext);
  if (fileExists(FLAGS_tls_ticket_seed_path)) {
    thriftServer.watchTicketPathForChanges(FLAGS_tls_ticket_seed_path, true);
  }

  return;
}

} // namespace openr
