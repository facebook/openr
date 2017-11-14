/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <glog/logging.h>
#include <map>
#include <memory>

#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/if/gen-cpp2/KnownKeys_types.h>

namespace openr {

//
// For the public key store, it allows for saving the JSON object
// dynamically, as new keys are being added removed. It's users
// responsiblity to sync they public key cache to disk.
//
class KnownKeysStore final {
 public:
  KnownKeysStore() = default;

  // the c-tor will load known keys from disk file
  explicit KnownKeysStore(const std::string knownKeysFilePath);

  // get peer's public key from memory cache
  std::string getKeyByName(const std::string& peerName) const;

  // unconditionally set peer key in memory cache
  void setKeyByName(const std::string& peerName, const std::string& peerKey);

  // save known public keys to disk, return true on success
  bool saveKeysToDisk() const;

 private:
  // make non-copyable
  KnownKeysStore(const KnownKeysStore&) = delete;
  KnownKeysStore& operator=(const KnownKeysStore&) = delete;

  //
  // Private state
  //

  // the path to the known keys file
  const std::string knownKeysFilePath_;

  // public key store
  thrift::KnownKeys knownKeys_;
};
} // namespace openr
