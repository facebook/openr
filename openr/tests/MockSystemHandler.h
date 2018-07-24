#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/SystemService.h>

namespace openr {

class MockSystemHandler final : public thrift::SystemServiceSvIf {
 public:
   MockSystemHandler() {}
   virtual ~MockSystemHandler() {}

   void getIfaceAddresses(
     std::vector< ::openr::thrift::IpPrefix>& _return,
     std::unique_ptr<std::string> iface, int16_t family, int16_t scope)
     override;

   void syncIfaceAddresses(
     std::unique_ptr<std::string> iface, int16_t family, int16_t scope,
     std::unique_ptr<std::vector< ::openr::thrift::IpPrefix>> addrs) override;

   void removeIfaceAddresses(
     std::unique_ptr<std::string> iface,
     std::unique_ptr<std::vector< ::openr::thrift::IpPrefix>> addrs) override;
};
} // namespace openr
