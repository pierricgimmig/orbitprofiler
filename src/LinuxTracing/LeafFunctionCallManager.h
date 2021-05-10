// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LINUX_TRACING_LEAF_FUNCTION_UTILS_H_
#define LINUX_TRACING_LEAF_FUNCTION_UTILS_H_

#include "LibunwindstackMaps.h"
#include "LibunwindstackUnwinder.h"
#include "PerfEvent.h"

namespace orbit_linux_tracing {

// This class provides the `PatchLeafFunctionCaller` method to fix a frame-pointer based callchain,
// where the leaf function does not have frame-pointers.
// Note that, this is wrapped in a class to allow tests to mock this implementation.
class LeafFunctionCallManager {
 public:
  virtual ~LeafFunctionCallManager() = default;

  // Computes the actual caller of a leaf function (that may not have frame-pointers) based on
  // libunwindstack and modifies the given callchain event, if needed.
  // In case of any unwinding error (either from libunwindstack or in the frame-pointer based
  // callchain), `false` will be returned and the event remains untouched.
  // If the innermost frame has frame-pointers, this function will return `true` and keeps the
  // callchain event untouched.
  // Otherwise, that is the caller of the leaf function is missing and there are no unwinding
  // errors, the callchain event gets updated, such that it contains the missing caller, and `true`
  // will be returned.
  // Note that, the address of the caller address is computed by decreasing the return address by
  // one in libunwindstack, to match the format of perf_event_open.
  virtual bool PatchLeafFunctionCaller(CallchainSamplePerfEvent* event,
                                       LibunwindstackMaps* current_maps,
                                       LibunwindstackUnwinder* unwinder);
};

}  //  namespace orbit_linux_tracing

#endif  // LINUX_TRACING_LEAF_FUNCTION_UTILS_H_
