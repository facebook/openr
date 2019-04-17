/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

extern "C" {
#include <authsae/ampe.h>
#include <authsae/sae.h>
}

// This file is intended for typedefs of authsae types whose naming is confusing
// in the context of fbmeshd.

using authsae_sae_config = sae_config;
using authsae_mesh_node = mesh_node;
using authsae_meshd_config = meshd_config;
