/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace py openr.PersistentStore

enum StoreRequestType {
  STORE = 1;
  LOAD = 2;
  ERASE = 3;
}

struct StoreRequest {
  1: StoreRequestType requestType;
  2: string key;
  3: binary data;    // Will be present only if `requestType == STORE`
}

struct StoreResponse {
  1: bool success = 0;
  2: string key;
  3: binary data;    // Will be there only if `success` is true
}

struct StoreDatabase {
  // map <key -> data>
  1: map<string, binary> keyVals;
}
