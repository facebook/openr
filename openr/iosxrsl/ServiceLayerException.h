#pragma once

#include <stdexcept>

#include <folly/Format.h>

namespace openr {

class IosxrslException : public std::runtime_error {
 public:
  explicit IosxrslException(const std::string& exception)
      : std::runtime_error(
            folly::sformat("Iosxrsl exception: {} ", exception)) {}
};
} // namespace openr
