/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KnownKeysStore.h"

#include <fstream>

#include <fbzmq/zmq/Zmq.h>
#include <folly/FileUtil.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

using namespace std;

namespace openr {

KnownKeysStore::KnownKeysStore(const std::string knownKeysFilePath)
    : knownKeysFilePath_(knownKeysFilePath) {
  std::string knownKeysStr;
  if (!folly::readFile(knownKeysFilePath_.c_str(), knownKeysStr)) {
    LOG(ERROR) << "Failed reading known keys, file might be missing";
    return;
  }

  apache::thrift::SimpleJSONSerializer serializer;
  try {
    knownKeys_ = fbzmq::util::readThriftObjStr<thrift::KnownKeys>(
        knownKeysStr, serializer);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Could not parse known keys from file " << knownKeysFilePath_;
  }
}

string
KnownKeysStore::getKeyByName(const string& peerName) const {
  return knownKeys_.keys.at(peerName);
}

void
KnownKeysStore::setKeyByName(const string& peerName, const string& peerKey) {
  knownKeys_.keys[peerName] = peerKey;
}

bool
KnownKeysStore::saveKeysToDisk() const {
  apache::thrift::SimpleJSONSerializer serializer;
  std::string knownKeysStr;
  try {
    knownKeysStr = fbzmq::util::writeThriftObjStr(knownKeys_, serializer);
  } catch (const std::exception& e) {
    LOG(ERROR) << "Could not serialize known keys";
  }

  return folly::writeFile(knownKeysStr, knownKeysFilePath_.c_str());
}
} // namespace openr
