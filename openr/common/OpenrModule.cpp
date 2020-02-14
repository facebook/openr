/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "openr/common/OpenrModule.h"

#include <folly/Format.h>

namespace openr {

OpenrModule::OpenrModule(
    const std::string& nodeName,
    const thrift::OpenrModuleType type,
    fbzmq::Context& zmqContext)
    : moduleType(type),
      moduleName(apache::thrift::TEnumTraits<thrift::OpenrModuleType>::findName(
          moduleType)) {}

} // namespace openr
