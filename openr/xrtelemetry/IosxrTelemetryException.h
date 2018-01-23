#pragma once

#include <stdexcept>

#include <folly/Format.h>

namespace openr {

class IosxrTelemetryException : public std::runtime_error {
 public:
  explicit IosxrTelemetryException(const std::string& exception)
      : std::runtime_error(
            folly::sformat("IosxrTelemetry exception: {} ", exception)) {}
};
} // namespace openr
