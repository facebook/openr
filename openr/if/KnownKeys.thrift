/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.KnownKeys

struct KnownKeys {
  1: required map<string /* peerName */,
                  binary /* peerPubKey */> keys
}

// curve public/private key pair
struct CurveKeyPair {
  1: binary privateKey
  2: binary publicKey
}
