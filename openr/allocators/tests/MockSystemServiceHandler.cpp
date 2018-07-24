#include "MockSystemServiceHandler.h"

#include <openr/common/Util.h>

namespace openr {

void MockSystemServiceHandler::getIfaceAddresses(
  std::vector< ::openr::thrift::IpPrefix>& _return,
  std::unique_ptr<std::string> iface, int16_t family, int16_t) {
    _return.clear();
    auto prefixes = getIfacePrefixes(*iface, family);
    for (const auto& prefix : prefixes) {
      _return.emplace_back(toIpPrefix(prefix));
    }
}
} // namespace openr
