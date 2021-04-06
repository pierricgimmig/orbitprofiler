// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LINUX_TRACING_THREAD_STATE_MANAGER_H_
#define LINUX_TRACING_THREAD_STATE_MANAGER_H_

#include <stdint.h>
#include <sys/types.h>

#include <optional>
#include <vector>

#include "OrbitBase/Logging.h"
#include "absl/container/flat_hash_map.h"
#include "capture.pb.h"

namespace orbit_linux_tracing {

// ThreadStateManager stores the state of threads, handles the state transitions,
// builds and returns ThreadStateSlices.
// The following diagram shows the relationship between the states and the tracepoints.
// Note that, for some state transitions, multiple tracepoints could be used
// (e.g., both sched:sched_waking and sched:sched_wakeup for "not runnable" to "runnable").
// The diagram indicates them all but we only use the ones not in parentheses.
// Also note we don't have a transition out of the diagram for a thread that exits.
// Instead, a thread that has exited will remain "not runnable" with state "dead"
// or sometimes "zombie". This is mostly for simplicity reasons,
// as a thread that exits first goes through sched:sched_process_exit,
// but then still goes through one or often multiple sched:sched_switches.
//
//       task:task_newtask                             sched:sched_switch(in)
//   (OR sched:sched_wakeup_new)    ------------ -------------------------------> -----------
// -------------------------------> | Runnable |                                  | Running |
//                                  ------------ <------------------------------- -----------
//                                       ^            sched:sched_switch(out)       ^  |
//                                       |             with prev_state=='R'         .  |
//                                       |                                          .  |
//                                       |                   sched:sched_switch(in) .  |
//                                       |               ---------------- . . . . . .  |
//                                       |               | Not runnable |              |
//                                       --------------- | incl. exited | <-------------
//                                sched:sched_wakeup     ----------------    sched_switch(out)
//                             (OR sched:sched_waking)                      with prev_state!='R'
//                                                                   (ALSO sched:sched_process_exit)

class ThreadStateManager {
 public:
  void OnInitialState(uint64_t timestamp_ns, pid_t pid, pid_t tid,
                      orbit_grpc_protos::ThreadStateSlice::ThreadState state);
  void OnNewTask(uint64_t timestamp_ns, pid_t pid, pid_t tid);
  [[nodiscard]] std::optional<orbit_grpc_protos::ThreadStateSlice> OnSchedWakeup(
      uint64_t timestamp_ns, pid_t pid, pid_t tid);
  [[nodiscard]] std::optional<orbit_grpc_protos::ThreadStateSlice> OnSchedSwitchIn(
      uint64_t timestamp_ns, pid_t pid, pid_t tid);
  [[nodiscard]] std::optional<orbit_grpc_protos::ThreadStateSlice> OnSchedSwitchOut(
      uint64_t timestamp_ns, pid_t pid, pid_t tid,
      orbit_grpc_protos::ThreadStateSlice::ThreadState new_state);
  [[nodiscard]] std::vector<orbit_grpc_protos::ThreadStateSlice> OnCaptureFinished(
      uint64_t timestamp_ns);

 private:
  struct OpenState {
    OpenState(pid_t pid, orbit_grpc_protos::ThreadStateSlice::ThreadState state,
              uint64_t begin_timestamp_ns)
        : pid{pid}, state{state}, begin_timestamp_ns{begin_timestamp_ns} {}
    pid_t pid;
    orbit_grpc_protos::ThreadStateSlice::ThreadState state;
    uint64_t begin_timestamp_ns;
  };

  absl::flat_hash_map<pid_t, OpenState> tid_open_states_;
};

}  // namespace orbit_linux_tracing

#endif  // LINUX_TRACING_THREAD_STATE_MANAGER_H_
