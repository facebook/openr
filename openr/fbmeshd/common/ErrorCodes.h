/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#define R_SUCCESS 0
#define ERR_RTNETLINK_ALLOCATE_SOCKET 1 // Failed to allocate rtnetlink socket
#define ERR_IFINDEX_NOT_FOUND 2 // Failed to find ifIndex
#define ERR_RTNL_ALLOCATE_OBJ 3 // Failed to allocate rtnl link object
#define ERR_CHANGE_LINKSTATE 4 // Failed to change link state in kernel
#define ERR_IFNAME 5
#define ERR_NETLINK_OTHER 17 // Other Netlink errors
#define ERR_UNSUPPORTED_CONFIG 18 // Provided mesh config is not supported
#define ERR_INVALID_ARGUMENT_VALUE 19 // Provided argument value is invalid
#define ERR_AUTHSAE 20 // Failure coming from authsae during authentication

typedef int status_t;
