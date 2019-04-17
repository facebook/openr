/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/fbmeshd/nl/NetlinkCallbackHandle.h>

namespace openr {
namespace fbmeshd {

using GenericNetlinkCallbackHandle =
    NetlinkCallbackHandle<GenericNetlinkMessage>;

} // namespace fbmeshd
} // namespace openr
