// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LINUX_TRACING_PERF_EVENT_H_
#define LINUX_TRACING_PERF_EVENT_H_

#include <asm/perf_regs.h>
#include <linux/perf_event.h>
#include <string.h>
#include <sys/types.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Function.h"
#include "KernelTracepoints.h"
#include "OrbitBase/MakeUniqueForOverwrite.h"
#include "PerfEventRecords.h"

namespace orbit_linux_tracing {

class PerfEventVisitor;

// This base class is used to do processing of different perf_event_open events
// using the visitor pattern. To avoid unnecessary copies, the data of the
// perf_event_open records will be copied from the ring buffer directly into the
// concrete subclass (depending on the event type), in general into a
// "ring_buffer_record" field.

class PerfEvent {
 public:
  virtual ~PerfEvent() = default;
  virtual uint64_t GetTimestamp() const = 0;
  virtual void Accept(PerfEventVisitor* visitor) = 0;

  static constexpr int kNotOrderedInAnyFileDescriptor = -1;
  void SetOrderedInFileDescriptor(int fd) { ordered_in_file_descriptor_ = fd; }
  int GetOrderedInFileDescriptor() const { return ordered_in_file_descriptor_; }

 private:
  int ordered_in_file_descriptor_ = kNotOrderedInAnyFileDescriptor;
};

std::array<uint64_t, PERF_REG_X86_64_MAX> perf_event_sample_regs_user_all_to_register_array(
    const perf_event_sample_regs_user_all& regs);

class ContextSwitchPerfEvent : public PerfEvent {
 public:
  perf_event_context_switch ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }

  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  bool IsSwitchOut() const { return ring_buffer_record.header.misc & PERF_RECORD_MISC_SWITCH_OUT; }
  bool IsSwitchIn() const { return !IsSwitchOut(); }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class SystemWideContextSwitchPerfEvent : public PerfEvent {
 public:
  perf_event_context_switch_cpu_wide ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }

  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  bool IsSwitchOut() const { return ring_buffer_record.header.misc & PERF_RECORD_MISC_SWITCH_OUT; }

  bool IsSwitchIn() const { return !IsSwitchOut(); }

  // Careful: even if PERF_RECORD_SWITCH_CPU_WIDE events carry information on
  // both the thread being de-scheduled and the one being scheduled (if the cpu
  // is switching from a thread to another and not from/to an idle state), two
  // separate PERF_RECORD_SWITCH_CPU_WIDE are still generated, one for the
  // switch-out and one for the switch-in. Therefore, prefer GetPid/Tid and
  // IsSwitchOut/In to GetPrev/NextPid/Tid.

  pid_t GetPrevPid() const { return IsSwitchOut() ? GetPid() : ring_buffer_record.next_prev_pid; }

  pid_t GetPrevTid() const { return IsSwitchOut() ? GetTid() : ring_buffer_record.next_prev_tid; }

  pid_t GetNextPid() const { return IsSwitchOut() ? ring_buffer_record.next_prev_pid : GetPid(); }

  pid_t GetNextTid() const { return IsSwitchOut() ? ring_buffer_record.next_prev_tid : GetTid(); }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class ForkPerfEvent : public PerfEvent {
 public:
  perf_event_fork_exit ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.pid; }
  pid_t GetParentPid() const { return ring_buffer_record.ppid; }
  pid_t GetTid() const { return ring_buffer_record.tid; }
  pid_t GetParentTid() const { return ring_buffer_record.ptid; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class ExitPerfEvent : public PerfEvent {
 public:
  perf_event_fork_exit ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.pid; }
  pid_t GetParentPid() const { return ring_buffer_record.ppid; }
  pid_t GetTid() const { return ring_buffer_record.tid; }
  pid_t GetParentTid() const { return ring_buffer_record.ptid; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class LostPerfEvent : public PerfEvent {
 public:
  perf_event_lost ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  uint64_t GetNumLost() const { return ring_buffer_record.lost; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  void SetPreviousTimestamp(uint64_t previous_timestamp) {
    previous_timestamp_ = previous_timestamp;
  }
  [[nodiscard]] uint64_t GetPreviousTimestamp() const { return previous_timestamp_; }

 private:
  // The last timestamp read from the ring buffer before the PERF_RECORD_LOST.
  uint64_t previous_timestamp_ = 0;
};

// This class doesn't correspond to any event generated by perf_event_open. Rather, these events are
// produced by PerfEventProcessor. We need them to be part of the same PerfEvent hierarchy.
class DiscardedPerfEvent : public PerfEvent {
 public:
  DiscardedPerfEvent(uint64_t begin_timestamp_ns, uint64_t end_timestamp_ns)
      : begin_timestamp_ns_{begin_timestamp_ns}, end_timestamp_ns_{end_timestamp_ns} {}

  uint64_t GetTimestamp() const override { return GetEndTimestampNs(); }

  void Accept(PerfEventVisitor* visitor) override;

  uint64_t GetBeginTimestampNs() const { return begin_timestamp_ns_; }
  uint64_t GetEndTimestampNs() const { return end_timestamp_ns_; }

 private:
  uint64_t begin_timestamp_ns_;
  uint64_t end_timestamp_ns_;
};

struct dynamically_sized_perf_event_sample_stack_user {
  uint64_t dyn_size;
  std::unique_ptr<char[]> data;

  explicit dynamically_sized_perf_event_sample_stack_user(uint64_t dyn_size)
      : dyn_size{dyn_size}, data{make_unique_for_overwrite<char[]>(dyn_size)} {}
};

struct dynamically_sized_perf_event_stack_sample {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  perf_event_sample_regs_user_all regs;
  dynamically_sized_perf_event_sample_stack_user stack;

  explicit dynamically_sized_perf_event_stack_sample(uint64_t dyn_size) : stack{dyn_size} {}
};

class StackSamplePerfEvent : public PerfEvent {
 public:
  dynamically_sized_perf_event_stack_sample ring_buffer_record;

  explicit StackSamplePerfEvent(uint64_t dyn_size) : ring_buffer_record{dyn_size} {}

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }
  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  std::array<uint64_t, PERF_REG_X86_64_MAX> GetRegisters() const {
    return perf_event_sample_regs_user_all_to_register_array(ring_buffer_record.regs);
  }

  const char* GetStackData() const { return ring_buffer_record.stack.data.get(); }
  char* GetStackData() { return ring_buffer_record.stack.data.get(); }
  uint64_t GetStackSize() const { return ring_buffer_record.stack.dyn_size; }

 private:
};

class CallchainSamplePerfEvent : public PerfEvent {
 public:
  perf_event_callchain_sample_fixed ring_buffer_record;
  std::vector<uint64_t> ips;
  perf_event_sample_regs_user_all regs;
  dynamically_sized_perf_event_sample_stack_user stack;

  explicit CallchainSamplePerfEvent(uint64_t callchain_size, uint64_t dyn_stack_size)
      : ips(callchain_size), stack(dyn_stack_size) {
    ring_buffer_record.nr = callchain_size;
  }

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }
  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  uint64_t* GetCallchain() { return ips.data(); }
  const uint64_t* GetCallchain() const { return ips.data(); }

  uint64_t GetCallchainSize() const { return ring_buffer_record.nr; }

  [[nodiscard]] std::array<uint64_t, PERF_REG_X86_64_MAX> GetRegisters() const {
    return perf_event_sample_regs_user_all_to_register_array(regs);
  }

  [[nodiscard]] const char* GetStackData() const { return stack.data.get(); }
  [[nodiscard]] char* GetStackData() { return stack.data.get(); }
  [[nodiscard]] uint64_t GetStackSize() const { return stack.dyn_size; }
};

class AbstractUprobesPerfEvent {
 public:
  const Function* GetFunction() const { return function_; }
  void SetFunction(const Function* function) { function_ = function; }

 private:
  const Function* function_ = nullptr;
};

class UprobesPerfEvent : public PerfEvent, public AbstractUprobesPerfEvent {
 public:
  perf_event_sp_ip_arguments_8bytes_sample ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }
  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  // Get the stack pointer.
  uint64_t GetSp() const { return ring_buffer_record.regs.sp; }

  // Get the instruction pointer.
  uint64_t GetIp() const { return ring_buffer_record.regs.ip; }

  uint64_t GetReturnAddress() const { return ring_buffer_record.stack.top8bytes; }
};

class UretprobesPerfEvent : public PerfEvent, public AbstractUprobesPerfEvent {
 public:
  perf_event_ax_sample ring_buffer_record;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }
  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  // Get AX register which holds integer return value.
  // See https://wiki.osdev.org/System_V_ABI.
  uint64_t GetAx() const { return ring_buffer_record.regs.ax; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class MmapPerfEvent : public PerfEvent {
 public:
  MmapPerfEvent(int32_t pid, uint64_t timestamp, const perf_event_mmap_up_to_pgoff& mmap_event,
                std::string filename)
      : pid_{pid}, timestamp_{timestamp}, mmap_event_{mmap_event}, filename_{std::move(filename)} {}

  uint64_t GetTimestamp() const override { return timestamp_; }

  void Accept(PerfEventVisitor* visitor) override;

  [[nodiscard]] const std::string& filename() const { return filename_; }
  [[nodiscard]] int32_t pid() const { return pid_; }
  [[nodiscard]] uint64_t address() const { return mmap_event_.address; }
  [[nodiscard]] uint64_t length() const { return mmap_event_.length; }
  [[nodiscard]] uint64_t page_offset() const { return mmap_event_.page_offset; }

 private:
  int32_t pid_;
  uint64_t timestamp_;
  perf_event_mmap_up_to_pgoff mmap_event_;
  std::string filename_;
};

class GenericTracepointPerfEvent : public PerfEvent {
 public:
  explicit GenericTracepointPerfEvent() {}

  perf_event_raw_sample_fixed ring_buffer_record;

  void Accept(PerfEventVisitor* visitor) override;

  int32_t GetPid() const { return ring_buffer_record.sample_id.pid; }

  int32_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }
};

class TracepointPerfEvent : public PerfEvent {
 public:
  explicit TracepointPerfEvent(uint32_t size)
      : tracepoint_data{make_unique_for_overwrite<uint8_t[]>(size)} {}

  perf_event_raw_sample_fixed ring_buffer_record;
  std::unique_ptr<uint8_t[]> tracepoint_data;

  uint64_t GetTimestamp() const override { return ring_buffer_record.sample_id.time; }

  uint64_t GetStreamId() const { return ring_buffer_record.sample_id.stream_id; }

  uint32_t GetCpu() const { return ring_buffer_record.sample_id.cpu; }

  uint16_t GetTracepointId() const { return GetTracepointCommon().common_type; }

 protected:
  const tracepoint_common& GetTracepointCommon() const {
    return *reinterpret_cast<const tracepoint_common*>(tracepoint_data.get());
  }

  template <typename TracepointData>
  const TracepointData& GetTypedTracepointData() const {
    return *reinterpret_cast<const TracepointData*>(tracepoint_data.get());
  }
};

class TaskNewtaskPerfEvent : public TracepointPerfEvent {
 public:
  explicit TaskNewtaskPerfEvent(uint32_t tracepoint_size) : TracepointPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  // The tracepoint format calls this "pid" but it's effectively the thread id.
  // Note that ring_buffer_record.sample_id.pid and ring_buffer_record.sample_id.tid are NOT the pid
  // and tid of the new process/thread, but the ones of the process/thread that created this one.
  pid_t GetNewTid() const { return GetTypedTracepointData<task_newtask_tracepoint>().pid; }

  const char* GetComm() const { return GetTypedTracepointData<task_newtask_tracepoint>().comm; }
};

class TaskRenamePerfEvent : public TracepointPerfEvent {
 public:
  explicit TaskRenamePerfEvent(uint32_t tracepoint_size) : TracepointPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  // The tracepoint format calls this "pid" but it's effectively the thread id.
  // This should match ring_buffer_record.sample_id.tid.
  pid_t GetRenamedTid() const { return GetTypedTracepointData<task_rename_tracepoint>().pid; }

  const char* GetOldComm() const {
    return GetTypedTracepointData<task_rename_tracepoint>().oldcomm;
  }
  const char* GetNewComm() const {
    return GetTypedTracepointData<task_rename_tracepoint>().newcomm;
  }
};

class SchedSwitchPerfEvent : public TracepointPerfEvent {
 public:
  explicit SchedSwitchPerfEvent(uint32_t tracepoint_size) : TracepointPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  // As the tracepoint data does not include the pid of the process that the thread being switched
  // out belongs to, we use the pid set by perf_event_open in the corresponding generic field of the
  // PERF_RECORD_SAMPLE.
  // Note, though, that this value is -1 when the switch out is caused by the thread exiting.
  // This is not the case for GetPrevTid(), whose value is always correct as it comes directly from
  // the tracepoint data.
  pid_t GetPrevPidOrMinusOne() const { return ring_buffer_record.sample_id.pid; }

  const char* GetPrevComm() const {
    return GetTypedTracepointData<sched_switch_tracepoint>().prev_comm;
  }

  pid_t GetPrevTid() const { return GetTypedTracepointData<sched_switch_tracepoint>().prev_pid; }

  int64_t GetPrevState() const {
    return GetTypedTracepointData<sched_switch_tracepoint>().prev_state;
  }

  const char* GetNextComm() const {
    return GetTypedTracepointData<sched_switch_tracepoint>().next_comm;
  }

  pid_t GetNextTid() const { return GetTypedTracepointData<sched_switch_tracepoint>().next_pid; }
};

class SchedWakeupPerfEvent : public TracepointPerfEvent {
 public:
  explicit SchedWakeupPerfEvent(uint32_t tracepoint_size) : TracepointPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  pid_t GetWakerPid() const { return ring_buffer_record.sample_id.pid; }

  pid_t GetWakerTid() const { return ring_buffer_record.sample_id.tid; }

  // The tracepoint format calls this "pid" but it's effectively the thread id.
  pid_t GetWokenTid() const { return GetTypedTracepointData<sched_wakeup_tracepoint>().pid; }
};

class GpuPerfEvent : public TracepointPerfEvent {
 public:
  explicit GpuPerfEvent(uint32_t tracepoint_size) : TracepointPerfEvent(tracepoint_size) {}

  std::string ExtractTimelineString() const {
    int32_t data_loc = GetTimeline();
    int16_t data_loc_size = static_cast<int16_t>(data_loc >> 16);
    int16_t data_loc_offset = static_cast<int16_t>(data_loc & 0x00ff);

    std::vector<char> data_loc_data(data_loc_size);
    std::memcpy(&data_loc_data[0],
                reinterpret_cast<const char*>(tracepoint_data.get()) + data_loc_offset,
                data_loc_size);

    // While the string should be null terminated here, we make sure that it
    // actually is by adding a zero in the last position. In the case of
    // expected behavior, this is a no-op.
    data_loc_data[data_loc_data.size() - 1] = 0;
    return std::string(&data_loc_data[0]);
  }

  pid_t GetPid() const { return ring_buffer_record.sample_id.pid; }

  pid_t GetTid() const { return ring_buffer_record.sample_id.tid; }

  virtual uint32_t GetContext() const = 0;
  virtual uint32_t GetSeqno() const = 0;

 protected:
  virtual int32_t GetTimeline() const = 0;
};

class AmdgpuCsIoctlPerfEvent : public GpuPerfEvent {
 public:
  explicit AmdgpuCsIoctlPerfEvent(uint32_t tracepoint_size) : GpuPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  uint32_t GetContext() const override {
    return GetTypedTracepointData<amdgpu_cs_ioctl_tracepoint>().context;
  }

  uint32_t GetSeqno() const override {
    return GetTypedTracepointData<amdgpu_cs_ioctl_tracepoint>().seqno;
  }

 protected:
  int32_t GetTimeline() const override {
    return GetTypedTracepointData<amdgpu_cs_ioctl_tracepoint>().timeline;
  }
};

class AmdgpuSchedRunJobPerfEvent : public GpuPerfEvent {
 public:
  explicit AmdgpuSchedRunJobPerfEvent(uint32_t tracepoint_size) : GpuPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  uint32_t GetContext() const override {
    return GetTypedTracepointData<amdgpu_sched_run_job_tracepoint>().context;
  }

  uint32_t GetSeqno() const override {
    return GetTypedTracepointData<amdgpu_sched_run_job_tracepoint>().seqno;
  }

 protected:
  int32_t GetTimeline() const override {
    return GetTypedTracepointData<amdgpu_sched_run_job_tracepoint>().timeline;
  }
};

class DmaFenceSignaledPerfEvent : public GpuPerfEvent {
 public:
  explicit DmaFenceSignaledPerfEvent(uint32_t tracepoint_size) : GpuPerfEvent(tracepoint_size) {}

  void Accept(PerfEventVisitor* visitor) override;

  uint32_t GetContext() const override {
    return GetTypedTracepointData<dma_fence_signaled_tracepoint>().context;
  }

  uint32_t GetSeqno() const override {
    return GetTypedTracepointData<dma_fence_signaled_tracepoint>().seqno;
  }

 protected:
  int32_t GetTimeline() const override {
    return GetTypedTracepointData<dma_fence_signaled_tracepoint>().timeline;
  }
};

}  // namespace orbit_linux_tracing

#endif  // LINUX_TRACING_PERF_EVENT_H_
