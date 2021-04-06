// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "SwitchesStatesNamesVisitor.h"

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>

#include <utility>
#include <vector>

#include "LinuxTracing/TracerListener.h"
#include "OrbitBase/Logging.h"

namespace orbit_linux_tracing {

using orbit_grpc_protos::SchedulingSlice;
using orbit_grpc_protos::ThreadName;
using orbit_grpc_protos::ThreadStateSlice;

void SwitchesStatesNamesVisitor::ProcessInitialTidToPidAssociation(pid_t tid, pid_t pid) {
  bool new_insertion = tid_to_pid_association_.insert_or_assign(tid, pid).second;
  if (!new_insertion) {
    ERROR("Overwriting previous pid for tid %d with initial pid %d", tid, pid);
  }
}

void SwitchesStatesNamesVisitor::visit(ForkPerfEvent* event) {
  pid_t pid = event->GetPid();
  pid_t tid = event->GetTid();
  bool new_insertion = tid_to_pid_association_.insert_or_assign(tid, pid).second;
  if (!new_insertion) {
    ERROR("Overwriting previous pid for tid %d with pid %d from PERF_RECORD_FORK", tid, pid);
  }
}

// We also use PERF_RECORD_EXIT to add associations between tids and pids. It might seem
// counter-intuitive but here is the rationale.
// At the beginning of the capture we might have sched:sched_switch events related to a thread that
// then exits before we have had the chance the retrieve the pid of the process that thread belongs
// to from /proc. Also, as explained below and elsewhere, for the context switches out of a cpu on
// thread exit the pid field of the PERF_RECORD_SAMPLE has value -1. In such special cases we can
// still use the pid from PERF_RECORD_EXIT and update the association just in time, as
// PERF_RECORD_EXIT events precede context switches with pid -1.
void SwitchesStatesNamesVisitor::visit(ExitPerfEvent* event) {
  pid_t pid = event->GetPid();
  pid_t tid = event->GetTid();
  tid_to_pid_association_.insert_or_assign(tid, pid);
  // Don't log an error on overwrite, as it's expected that the pid was already known.
}

bool SwitchesStatesNamesVisitor::TidMatchesPidFilter(pid_t tid) {
  if (thread_state_pid_filter_ == kPidFilterNoThreadState) {
    return false;
  }

  auto tid_to_pid_it = tid_to_pid_association_.find(tid);
  if (tid_to_pid_it == tid_to_pid_association_.end()) {
    return false;
  }

  return tid_to_pid_it->second == thread_state_pid_filter_;
}

std::optional<pid_t> SwitchesStatesNamesVisitor::GetPidOfTid(pid_t tid) {
  auto tid_to_pid_it = tid_to_pid_association_.find(tid);
  if (tid_to_pid_it == tid_to_pid_association_.end()) {
    return std::nullopt;
  }
  return tid_to_pid_it->second;
}

void SwitchesStatesNamesVisitor::ProcessInitialState(uint64_t timestamp_ns, pid_t tid,
                                                     char state_char) {
  if (!TidMatchesPidFilter(tid)) {
    return;
  }

  std::optional<ThreadStateSlice::ThreadState> initial_state = GetThreadStateFromChar(state_char);
  if (!initial_state.has_value()) {
    ERROR("Parsing thread state char '%c' for tid %d", state_char, tid);
    return;
  }
  state_manager_.OnInitialState(timestamp_ns, thread_state_pid_filter_, tid, initial_state.value());
}

void SwitchesStatesNamesVisitor::visit(TaskNewtaskPerfEvent* event) {
  std::optional<pid_t> new_pid = GetPidOfTid(event->GetNewTid());
  ThreadName thread_name;
  thread_name.set_pid(new_pid.value_or(-1));
  thread_name.set_tid(event->GetNewTid());
  thread_name.set_name(event->GetComm());
  thread_name.set_timestamp_ns(event->GetTimestamp());
  listener_->OnThreadName(std::move(thread_name));

  if (!TidMatchesPidFilter(event->GetNewTid())) {
    return;
  }
  state_manager_.OnNewTask(event->GetTimestamp(), thread_state_pid_filter_, event->GetNewTid());
}

void SwitchesStatesNamesVisitor::visit(SchedSwitchPerfEvent* event) {
  // Note that context switches with tid 0 are associated with idle CPU, so we never consider them.

  // Process the context switch out for scheduling slices.
  if (produce_scheduling_slices_ && event->GetPrevTid() != 0) {
    // TracepointPerfEvent::ring_buffer_record.sample_id.pid (which doesn't come from the tracepoint
    // data, but from the generic field of the PERF_RECORD_SAMPLE) is the pid of the process that
    // the thread being switched out belongs to. But when the switch out is caused by the thread
    // exiting, it has value -1. In such cases, use the association between tid and pid that we keep
    // internally to obtain the process id.
    pid_t prev_pid = event->GetPrevPidOrMinusOne();
    if (prev_pid == -1) {
      if (std::optional<pid_t> fallback_prev_pid = GetPidOfTid(event->GetPrevTid());
          fallback_prev_pid.has_value()) {
        prev_pid = fallback_prev_pid.value();
      }
    }
    std::optional<SchedulingSlice> scheduling_slice = switch_manager_.ProcessContextSwitchOut(
        prev_pid, event->GetPrevTid(), event->GetCpu(), event->GetTimestamp());
    if (scheduling_slice.has_value()) {
      CHECK(listener_ != nullptr);
      if (scheduling_slice->pid() == -1) {
        ERROR("SchedulingSlice with unknown pid");
      }
      listener_->OnSchedulingSlice(std::move(scheduling_slice.value()));
    }
  }

  // Process the context switch in for scheduling slices.
  if (produce_scheduling_slices_ && event->GetNextTid() != 0) {
    std::optional<pid_t> next_pid = GetPidOfTid(event->GetNextTid());
    switch_manager_.ProcessContextSwitchIn(next_pid, event->GetNextTid(), event->GetCpu(),
                                           event->GetTimestamp());
  }

  // Process the context switch out for thread state.
  if (event->GetPrevTid() != 0 && TidMatchesPidFilter(event->GetPrevTid())) {
    ThreadStateSlice::ThreadState new_state = GetThreadStateFromBits(event->GetPrevState());
    std::optional<ThreadStateSlice> out_slice = state_manager_.OnSchedSwitchOut(
        event->GetTimestamp(), thread_state_pid_filter_, event->GetPrevTid(), new_state);
    if (out_slice.has_value()) {
      CHECK(listener_ != nullptr);
      listener_->OnThreadStateSlice(std::move(out_slice.value()));
      if (thread_state_counter_ != nullptr) {
        ++(*thread_state_counter_);
      }
    }
  }

  // Process the context switch in for thread state.
  if (event->GetNextTid() != 0 && TidMatchesPidFilter(event->GetNextTid())) {
    std::optional<ThreadStateSlice> in_slice = state_manager_.OnSchedSwitchIn(
        event->GetTimestamp(), thread_state_pid_filter_, event->GetNextTid());
    if (in_slice.has_value()) {
      CHECK(listener_ != nullptr);
      listener_->OnThreadStateSlice(std::move(in_slice.value()));
      if (thread_state_counter_ != nullptr) {
        ++(*thread_state_counter_);
      }
    }
  }
}

void SwitchesStatesNamesVisitor::visit(SchedWakeupPerfEvent* event) {
  if (!TidMatchesPidFilter(event->GetWokenTid())) {
    return;
  }

  std::optional<ThreadStateSlice> state_slice = state_manager_.OnSchedWakeup(
      event->GetTimestamp(), thread_state_pid_filter_, event->GetWokenTid());
  if (state_slice.has_value()) {
    CHECK(listener_ != nullptr);
    listener_->OnThreadStateSlice(std::move(state_slice.value()));
    if (thread_state_counter_ != nullptr) {
      ++(*thread_state_counter_);
    }
  }
}

void SwitchesStatesNamesVisitor::ProcessRemainingOpenStates(uint64_t timestamp_ns) {
  std::vector<ThreadStateSlice> state_slices = state_manager_.OnCaptureFinished(timestamp_ns);
  for (ThreadStateSlice& slice : state_slices) {
    CHECK(listener_ != nullptr);
    listener_->OnThreadStateSlice(slice);
    if (thread_state_counter_ != nullptr) {
      ++(*thread_state_counter_);
    }
  }
}

void SwitchesStatesNamesVisitor::visit(TaskRenamePerfEvent* event) {
  std::optional<pid_t> renamed_pid = GetPidOfTid(event->GetRenamedTid());
  ThreadName thread_name;
  thread_name.set_pid(renamed_pid.value_or(-1));
  thread_name.set_tid(event->GetRenamedTid());
  thread_name.set_name(event->GetNewComm());
  thread_name.set_timestamp_ns(event->GetTimestamp());
  listener_->OnThreadName(std::move(thread_name));
}

// Associates a ThreadStateSlice::ThreadState to a thread state character retrieved from
// /proc/<pid>/stat or the `ps` command. The possible characters were manually obtained from
// /sys/kernel/debug/tracing/events/sched/sched_switch/format and compared with the ones listed in
// https://man7.org/linux/man-pages/man5/proc.5.html and
// https://www.man7.org/linux/man-pages/man1/ps.1.html#PROCESS_STATE_CODES to make sure we are not
// missing any additional valid one.
std::optional<ThreadStateSlice::ThreadState> SwitchesStatesNamesVisitor::GetThreadStateFromChar(
    char c) {
  switch (c) {
    case 'R':
      return ThreadStateSlice::kRunnable;
    case 'S':
      return ThreadStateSlice::kInterruptibleSleep;
    case 'D':
      return ThreadStateSlice::kUninterruptibleSleep;
    case 'T':
      return ThreadStateSlice::kStopped;
    case 't':
      return ThreadStateSlice::kTraced;
    case 'X':
      return ThreadStateSlice::kDead;
    case 'Z':
      return ThreadStateSlice::kZombie;
    case 'P':
      // Note that 'P' (Parked) is only valid from Linux 3.9 to 3.13, but we still include it as it
      // is mentioned in /sys/kernel/debug/tracing/events/sched/sched_switch/format and in
      // https://github.com/torvalds/linux/blob/master/fs/proc/array.c.
      return ThreadStateSlice::kParked;
    case 'I':
      // 'I' (Idle) only applies to kernel threads. See
      // https://github.com/torvalds/linux/commit/06eb61844d841d0032a9950ce7f8e783ee49c0d0.
      return ThreadStateSlice::kIdle;
    default:
      return std::nullopt;
  }
}

// Associates a ThreadStateSlice::ThreadState to the bits of the prev_state field of the
// sched:sched_switch tracepoint. The association is given away by "print fmt" in
// /sys/kernel/debug/tracing/events/sched/sched_switch/format or by
// https://github.com/torvalds/linux/blob/master/fs/proc/array.c.
ThreadStateSlice::ThreadState SwitchesStatesNamesVisitor::GetThreadStateFromBits(uint64_t bits) {
  if (__builtin_popcountl(bits & 0xFF) > 1) {
    ERROR("The thread state mask %#lx is a combination of states, reporting only the first",
          bits & 0xFF);
  }
  if ((bits & 0x01) != 0) return ThreadStateSlice::kInterruptibleSleep;
  if ((bits & 0x02) != 0) return ThreadStateSlice::kUninterruptibleSleep;
  if ((bits & 0x04) != 0) return ThreadStateSlice::kStopped;
  if ((bits & 0x08) != 0) return ThreadStateSlice::kTraced;
  if ((bits & 0x10) != 0) return ThreadStateSlice::kDead;
  if ((bits & 0x20) != 0) return ThreadStateSlice::kZombie;
  if ((bits & 0x40) != 0) return ThreadStateSlice::kParked;
  if ((bits & 0x80) != 0) return ThreadStateSlice::kIdle;
  return ThreadStateSlice::kRunnable;
}

}  // namespace orbit_linux_tracing
