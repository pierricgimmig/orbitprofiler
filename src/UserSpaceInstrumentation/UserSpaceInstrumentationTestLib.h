// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef USER_SPACE_INSTRUMENTATION_TEST_LIB_H_
#define USER_SPACE_INSTRUMENTATION_TEST_LIB_H_

#include <cstdint>

// This library is merely used in tests: The tests inject a binary produced by this code into its
// child and uses the functions defined here.

// Returns 42.
extern "C" int TrivialFunction();

// Returns sum of the parameters.
extern "C" uint64_t TrivialSum(uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3, uint64_t p4,
                               uint64_t p5);

// Uses printf to log.
extern "C" void TrivialLog(uint64_t function_address);

#endif  // USER_SPACE_INSTRUMENTATION_TEST_LIB_H_