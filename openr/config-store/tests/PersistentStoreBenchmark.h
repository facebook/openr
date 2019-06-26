/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <benchmark/benchmark.h>

namespace openr {
void BM_PersistentStoreWrite(benchmark::State& state);
void BM_PersistentStoreLoad(benchmark::State& state);
void BM_PersistentStoreCreateDestroy(benchmark::State& state);
} // namespace openr
