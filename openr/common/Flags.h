/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

// openr config file
DECLARE_string(config);

// drain workflow flags
DECLARE_bool(override_drain_state);

// migration workflow flags
DECLARE_bool(enable_bgp_route_programming);
DECLARE_int32(bgp_thrift_port);

// bgp stateful ha
DECLARE_string(spr_ha_state_file);
DECLARE_bool(bgp_enable_stateful_ha);

// security related flags
DECLARE_bool(enable_secure_thrift_server);
DECLARE_string(x509_cert_path);
DECLARE_string(x509_key_path);
DECLARE_string(x509_ca_path);
DECLARE_string(tls_ticket_seed_path);
DECLARE_string(tls_ecc_curve_name);
DECLARE_string(tls_acceptable_peers);

// file storing thrift::RibPolicy
DECLARE_string(rib_policy_file);
