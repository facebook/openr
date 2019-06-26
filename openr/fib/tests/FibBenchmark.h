/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <benchmark/benchmark.h>

namespace openr {
void BM_Fib(benchmark::State& state);
} // namespace openr
