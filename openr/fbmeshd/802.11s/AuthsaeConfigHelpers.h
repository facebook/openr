/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Portability.h>

#include <openr/fbmeshd/802.11s/AuthsaeTypes.h>
#include <openr/fbmeshd/802.11s/NetInterface.h>

// These are helper functions for Nl80211Handler::initMesh() which are used to
// properly create the SAE (sae_config) and AMPE (mesh_node) configuration
// structs to be passed to authsae.

FOLLY_NODISCARD authsae_sae_config
createSaeConfig(const openr::fbmeshd::NetInterface& netif);
FOLLY_NODISCARD authsae_mesh_node* createMeshConfig(
    openr::fbmeshd::NetInterface& netif);

// These are helper functions for the helper functions; they are meant for
// internal use only. They are declared in this header in order to enable unit
// testing, which would be impossible with an anonymous namespace.
namespace openr {
namespace fbmeshd {
namespace internal {

void setSupportedBasicRates(
    int phyIndex, authsae_meshd_config* mconf, u16* rates, size_t rates_len);

} // namespace internal
} // namespace fbmeshd
} // namespace openr
