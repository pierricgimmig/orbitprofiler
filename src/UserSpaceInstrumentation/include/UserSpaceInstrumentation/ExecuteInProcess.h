// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef USER_SPACE_INSTRUMENTATION_EXECUTE_IN_PROCESS_H_
#define USER_SPACE_INSTRUMENTATION_EXECUTE_IN_PROCESS_H_

#include <sys/types.h>

#include <cstdint>
#include <string_view>

#include "OrbitBase/Result.h"

namespace orbit_user_space_instrumentation {

// Execute `function` from the library indentified by `handle` with the given parameters inside
// process `pid`. `function` can be any function taking up to six integer parameter and might return
// an integer.
// Assumes that we are attached to the process `pid` (using `AttachAndStopProcess`) and the library
// identified by `handle` is loaded into this process (using `DlopenInTracee`).
[[nodiscard]] ErrorMessageOr<uint64_t> ExecuteInProcess(pid_t pid, void* handle,
                                                        std::string_view function,
                                                        uint64_t param_1 = 0, uint64_t param_2 = 0,
                                                        uint64_t param_3 = 0, uint64_t param_4 = 0,
                                                        uint64_t param_5 = 0, uint64_t param_6 = 0);
}  // namespace orbit_user_space_instrumentation

#endif  // USER_SPACE_INSTRUMENTATION_EXECUTE_IN_PROCESS_H_