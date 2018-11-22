/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <openr/common/Util.h>
#include <openr/if/gen-cpp2/SystemService.h>

namespace openr {

class MockSystemServiceHandler final : public thrift::SystemServiceSvIf {
 public:
   MockSystemServiceHandler() {}
   virtual ~MockSystemServiceHandler() {}

   void getIfaceAddresses(
     std::vector< ::openr::thrift::IpPrefix>& _return,
     std::unique_ptr<std::string> iface, int16_t family, int16_t scope)
     override;
};

} // namespace openr
