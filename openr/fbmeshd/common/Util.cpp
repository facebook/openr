/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Util.h"

#include <glog/logging.h>

#include <folly/String.h>

DEFINE_bool(
    enable_short_names,
    false,
    "true if this AP uses short names (without the 'faceb00c' prefix");

namespace openr {
namespace fbmeshd {

std::string
macAddrToNodeName(folly::MacAddress macAddr) noexcept {
  // In: 00:11:22:33:44:55 (as folly::MacAddress)
  // Out: 001122334455 (as string)
  const uint8_t* macOctets = macAddr.bytes();
  std::string name = folly::sformat(
      "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
      macOctets[0],
      macOctets[1],
      macOctets[2],
      macOctets[3],
      macOctets[4],
      macOctets[5]);
  if (FLAGS_enable_short_names) {
    return name;
  }
  return "faceb00c-face-b00c-face-" + name;
}

folly::Optional<folly::MacAddress>
nodeNameToMacAddr(folly::StringPiece name) noexcept {
  // In: faceb00c-face-b00c-face-001122334455 (as string) or
  //     001122334455 (as string)
  // Out: 00:11:22:33:44:55 (as folly::MacAddress)
  if (name.size() != 36 && name.size() != 12) {
    LOG(ERROR) << "error parsing name: " << name;
    return folly::none;
  }
  name.removePrefix("faceb00c-face-b00c-face-");
  std::array<folly::StringPiece, 6> macFields;
  for (size_t i = 0; i < macFields.size(); i++) {
    macFields.at(i) = name.subpiece(i * 2, 2);
  }
  return folly::MacAddress{folly::join(":", macFields)};
}

} // namespace fbmeshd
} // namespace openr
