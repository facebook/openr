/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <openr/common/Flags.h>
#include <openr/if/gen-cpp2/Types_constants.h>

DEFINE_string(config, "", "OpenR config file path");
DEFINE_bool(
    override_drain_state,
    false,
    "If set, will override persistent store drain state with value passed in "
    "--assume_drained");
DEFINE_bool(
    enable_bgp_route_programming,
    true,
    "Enable programming routes with prefix type BGP to the system FIB");
DEFINE_int32(bgp_thrift_port, 2029, "port for thrift service");
DEFINE_string(
    spr_ha_state_file,
    "/dev/shm/spr_ha_state.txt",
    "File in which HA stateful information is stored across bgp restarts");
DEFINE_bool(bgp_enable_stateful_ha, true, "Is Bgp peer stateful HA required");
DEFINE_bool(
    enable_secure_thrift_server,
    false,
    "Flag to enable TLS for our thrift server");
DEFINE_string(
    x509_cert_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate path for the associated wangle::SSLContextConfig");
DEFINE_string(
    x509_key_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "key path for the associated wangle::SSLContextConfig. If unspecified, "
    "will use x509_cert_path");
DEFINE_string(
    x509_ca_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "certificate authority path for verifying peers");
DEFINE_string(
    tls_ticket_seed_path,
    "",
    "If we are running an SSL thrift server, this option specifies the "
    "TLS ticket seed file path to use for client session resumption");
DEFINE_string(
    tls_ecc_curve_name,
    "prime256v1",
    "If we are running an SSL thrift server, this option specifies the "
    "eccCurveName for the associated wangle::SSLContextConfig");
DEFINE_string(
    tls_acceptable_peers,
    "",
    "A comma separated list of strings. Strings are x509 common names to "
    "accept SSL connections from. If an empty string is provided, the server "
    "will accept connections from any authenticated peer.");
