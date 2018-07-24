#include "MockSystemHandler.h"

#include <openr/common/Util.h>

namespace openr {

void MockSystemHandler::getIfaceAddresses(
  std::vector< ::openr::thrift::IpPrefix>& _return,
  std::unique_ptr<std::string> iface, int16_t family, int16_t) {
    _return.clear();
    auto prefixes = getIfacePrefixes(*iface, family);
    for (const auto& prefix : prefixes) {
      _return.emplace_back(toIpPrefix(prefix));
    }
}

void MockSystemHandler::syncIfaceAddresses(
  std::unique_ptr<std::string> , int16_t , int16_t ,
  std::unique_ptr<std::vector< ::openr::thrift::IpPrefix>> ) {}

void MockSystemHandler::removeIfaceAddresses(
  std::unique_ptr<std::string> ,
  std::unique_ptr<std::vector< ::openr::thrift::IpPrefix>> ) {}
} // namespace openr
