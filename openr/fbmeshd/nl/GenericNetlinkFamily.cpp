/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GenericNetlinkFamily.h"

#include <openr/fbmeshd/nl/GenericNetlinkSocket.h>

using namespace openr::fbmeshd;

const GenericNetlinkFamily&
GenericNetlinkFamily::NL80211() {
  static const GenericNetlinkFamily nl80211{"nl80211"};
  return nl80211;
}

const GenericNetlinkFamily&
GenericNetlinkFamily::NLCTRL() {
  static const GenericNetlinkFamily nlctrl{"nlctrl"};
  return nlctrl;
}

GenericNetlinkFamily::GenericNetlinkFamily(std::string family)
    : family_{family} {}

GenericNetlinkFamily::operator int() const {
  if (!familyId_.hasValue()) {
    familyId_ = GenericNetlinkSocket{}.resolveGenericNetlinkFamily(family_);
  }
  return *familyId_;
}
