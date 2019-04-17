/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string>

#include <folly/SocketAddress.h>

namespace openr {
namespace fbmeshd {

// This is a wrapper class around the posix socket calls. Its very simply right
// now as it just has one use case so it could be improved lot in the future.
class Socket final {
 public:
  struct Result {
    bool success;
    std::string errorMsg;
  };

  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&&) = delete;
  Socket& operator=(Socket&&) = delete;

  Result connect(
      const std::string& interface,
      const folly::SocketAddress& address,
      const std::chrono::seconds& socketTimeout);

 private:
  int fd{-1};
};

} // namespace fbmeshd
} // namespace openr
