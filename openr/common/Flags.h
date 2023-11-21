/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>

// openr config file
DECLARE_string(config);

// [TO_BE_DEPRECATED]
// security related flags
DECLARE_string(x509_cert_path);
DECLARE_string(x509_key_path);
DECLARE_string(x509_ca_path);
DECLARE_string(tls_ticket_seed_path);
DECLARE_string(tls_ecc_curve_name);

// file storing thrift::RibPolicy
DECLARE_string(rib_policy_file);
