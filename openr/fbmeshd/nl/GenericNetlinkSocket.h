/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <netlink/errno.h> // @manual
#include <netlink/genl/ctrl.h> // @manual
#include <netlink/genl/genl.h> // @manual

#include <openr/fbmeshd/nl/GenericNetlinkFamily.h>
#include <openr/fbmeshd/nl/GenericNetlinkMessage.h>
#include <openr/fbmeshd/nl/NetlinkSocket.h>

namespace openr {
namespace fbmeshd {

/**
 * Netlink wrapper for a generic nl_sock
 */
class GenericNetlinkSocket : public NetlinkSocket<GenericNetlinkMessage> {
 public:
  GenericNetlinkSocket() : NetlinkSocket() {
    int err = genl_connect(sock_);
    CHECK_EQ(err, 0) << nl_geterror(err);

    setBufferSize(8192, 8192);
  }

 private:
  int
  resolveGenericNetlinkFamily(const std::string& name) {
    int id = genl_ctrl_resolve(sock_, name.c_str());
    LOG_IF(ERROR, id < 0) << "could not resolve family " << name;
    return id;
  }

  friend class GenericNetlinkFamily;
};

} // namespace fbmeshd
} // namespace openr
