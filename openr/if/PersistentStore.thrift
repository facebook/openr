/*
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

namespace cpp2 openr.thrift
namespace go openr.PersistentStore
namespace py openr.PersistentStore
namespace py3 openr.thrift
namespace lua openr.PersistentStore

struct StoreDatabase {
  // map <key -> data>
  1: map<string, binary> keyVals;
}
