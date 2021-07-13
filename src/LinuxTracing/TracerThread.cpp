// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "TracerThread.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/meta/type_traits.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "Function.h"
#include "Introspection/Introspection.h"
#include "LibunwindstackMaps.h"
#include "LibunwindstackUnwinder.h"
#include "LinuxTracing/TracerListener.h"
#include "LinuxTracingUtils.h"
#include "ObjectUtils/LinuxMap.h"
#include "OrbitBase/ExecutablePath.h"
#include "OrbitBase/GetProcessIds.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/MakeUniqueForOverwrite.h"
#include "OrbitBase/ThreadUtils.h"
#include "PerfEventOpen.h"
#include "PerfEventReaders.h"
#include "PerfEventRecords.h"
#include "tracepoint.pb.h"

namespace orbit_linux_tracing {

using orbit_base::GetAllPids;
using orbit_base::GetTidsOfProcess;
using orbit_grpc_protos::CaptureOptions;
using orbit_grpc_protos::InstrumentedFunction;
using orbit_grpc_protos::ModuleInfo;
using orbit_grpc_protos::ModulesSnapshot;
using orbit_grpc_protos::ThreadName;
using orbit_grpc_protos::ThreadNamesSnapshot;

TracerThread::TracerThread(const CaptureOptions& capture_options)
    : trace_context_switches_{capture_options.trace_context_switches()},
      target_pid_{capture_options.pid()},
      unwinding_method_{capture_options.unwinding_method()},
      trace_thread_state_{capture_options.trace_thread_state()},
      trace_gpu_driver_{capture_options.trace_gpu_driver()} {
  if (unwinding_method_ != CaptureOptions::kUndefined) {
    uint32_t stack_dump_size = capture_options.stack_dump_size();
    if (stack_dump_size > kMaxStackSampleUserSize || stack_dump_size == 0) {
      ERROR("Invalid sample stack dump size: %u; Reassigning to default: %u", stack_dump_size,
            kMaxStackSampleUserSize);
      stack_dump_size = kMaxStackSampleUserSize;
    }
    stack_dump_size_ = static_cast<uint16_t>(stack_dump_size);
    std::optional<uint64_t> sampling_period_ns =
        ComputeSamplingPeriodNs(capture_options.samples_per_second());
    FAIL_IF(!sampling_period_ns.has_value(), "Invalid sampling rate: %.1f",
            capture_options.samples_per_second());
    sampling_period_ns_ = sampling_period_ns.value();
  } else {
    sampling_period_ns_ = 0;
  }

  instrumented_functions_.reserve(capture_options.instrumented_functions_size());

  for (const InstrumentedFunction& instrumented_function :
       capture_options.instrumented_functions()) {
    uint64_t function_id = instrumented_function.function_id();
    instrumented_functions_.emplace_back(function_id, instrumented_function.file_path(),
                                         instrumented_function.file_offset());

    // Manual instrumentation.
    if (instrumented_function.function_type() == InstrumentedFunction::kTimerStart) {
      manual_instrumentation_config_.AddTimerStartFunctionId(function_id);
    } else if (instrumented_function.function_type() == InstrumentedFunction::kTimerStop) {
      manual_instrumentation_config_.AddTimerStopFunctionId(function_id);
    }
  }

  for (const orbit_grpc_protos::TracepointInfo& instrumented_tracepoint :
       capture_options.instrumented_tracepoint()) {
    orbit_grpc_protos::TracepointInfo info;
    info.set_name(instrumented_tracepoint.name());
    info.set_category(instrumented_tracepoint.category());
    instrumented_tracepoints_.emplace_back(info);
  }
}

namespace {
void CloseFileDescriptors(const std::vector<int>& fds) {
  for (int fd : fds) {
    close(fd);
  }
}

void CloseFileDescriptors(const absl::flat_hash_map<int32_t, int>& fds_per_cpu) {
  for (const auto& pair : fds_per_cpu) {
    close(pair.second);
  }
}
}  // namespace

void TracerThread::InitUprobesEventVisitor() {
  ORBIT_SCOPE_FUNCTION;
  maps_ = LibunwindstackMaps::ParseMaps(ReadMaps(target_pid_));
  unwinder_ = LibunwindstackUnwinder::Create();
  leaf_function_call_manager_ = std::make_unique<LeafFunctionCallManager>(stack_dump_size_);
  uprobes_unwinding_visitor_ = std::make_unique<UprobesUnwindingVisitor>(
      listener_, &function_call_manager_, &return_address_manager_, maps_.get(), unwinder_.get(),
      leaf_function_call_manager_.get());
  uprobes_unwinding_visitor_->SetUnwindErrorsAndDiscardedSamplesCounters(
      &stats_.unwind_error_count, &stats_.samples_in_uretprobes_count);
  event_processor_.AddVisitor(uprobes_unwinding_visitor_.get());
}

bool TracerThread::OpenUprobes(const orbit_linux_tracing::Function& function,
                               const std::vector<int32_t>& cpus,
                               absl::flat_hash_map<int32_t, int>* fds_per_cpu) {
  ORBIT_SCOPE_FUNCTION;
  const char* module = function.file_path().c_str();
  const uint64_t offset = function.file_offset();
  for (int32_t cpu : cpus) {
    int fd = uprobes_retaddr_event_open(module, offset, -1, cpu);
    if (fd < 0) {
      ERROR("Opening uprobe %s+%#" PRIx64 " on cpu %d", function.file_path(),
            function.file_offset(), cpu);
      return false;
    }
    (*fds_per_cpu)[cpu] = fd;
  }
  return true;
}

bool TracerThread::OpenUretprobes(const orbit_linux_tracing::Function& function,
                                  const std::vector<int32_t>& cpus,
                                  absl::flat_hash_map<int32_t, int>* fds_per_cpu) {
  ORBIT_SCOPE_FUNCTION;
  const char* module = function.file_path().c_str();
  const uint64_t offset = function.file_offset();
  for (int32_t cpu : cpus) {
    int fd = uretprobes_event_open(module, offset, -1, cpu);
    if (fd < 0) {
      ERROR("Opening uretprobe %s+%#" PRIx64 " on cpu %d", function.file_path(),
            function.file_offset(), cpu);
      return false;
    }
    (*fds_per_cpu)[cpu] = fd;
  }
  return true;
}

void TracerThread::AddUprobesFileDescriptors(
    const absl::flat_hash_map<int32_t, int>& uprobes_fds_per_cpu,
    const orbit_linux_tracing::Function& function) {
  ORBIT_SCOPE_FUNCTION;
  for (const auto [cpu, fd] : uprobes_fds_per_cpu) {
    uint64_t stream_id = perf_event_get_id(fd);
    uprobes_uretprobes_ids_to_function_.emplace(stream_id, &function);
    uprobes_ids_.insert(stream_id);
    tracing_fds_.push_back(fd);
  }
}

void TracerThread::AddUretprobesFileDescriptors(
    const absl::flat_hash_map<int32_t, int>& uretprobes_fds_per_cpu,
    const orbit_linux_tracing::Function& function) {
  ORBIT_SCOPE_FUNCTION;
  for (const auto [cpu, fd] : uretprobes_fds_per_cpu) {
    uint64_t stream_id = perf_event_get_id(fd);
    uprobes_uretprobes_ids_to_function_.emplace(stream_id, &function);
    uretprobes_ids_.insert(stream_id);
    tracing_fds_.push_back(fd);
  }
}

bool TracerThread::OpenUserSpaceProbes(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  bool uprobes_event_open_errors = false;

  absl::flat_hash_map<int32_t, std::vector<int>> uprobes_uretpobres_fds_per_cpu;
  for (const auto& function : instrumented_functions_) {
    absl::flat_hash_map<int32_t, int> uprobes_fds_per_cpu;
    absl::flat_hash_map<int32_t, int> uretprobes_fds_per_cpu;

    if (manual_instrumentation_config_.IsTimerStartFunction(function.function_id())) {
      // Only open uprobes for a "timer start" manual instrumentation function.
      if (!OpenUprobes(function, cpus, &uprobes_fds_per_cpu)) {
        CloseFileDescriptors(uprobes_fds_per_cpu);
        uprobes_event_open_errors = true;
        continue;
      }
    } else if (manual_instrumentation_config_.IsTimerStopFunction(function.function_id())) {
      // Only open uretprobes for a "timer stop" manual instrumentation
      // function.
      if (!OpenUretprobes(function, cpus, &uretprobes_fds_per_cpu)) {
        CloseFileDescriptors(uretprobes_fds_per_cpu);
        uprobes_event_open_errors = true;
        continue;
      }
    } else {
      // Open both uprobes and uretprobes for regular functions.
      bool success = OpenUprobes(function, cpus, &uprobes_fds_per_cpu) &&
                     OpenUretprobes(function, cpus, &uretprobes_fds_per_cpu);
      if (!success) {
        CloseFileDescriptors(uprobes_fds_per_cpu);
        CloseFileDescriptors(uretprobes_fds_per_cpu);
        uprobes_event_open_errors = true;
        continue;
      }
    }

    // Uretprobe need to be enabled before uprobes as we support temporarily
    // not having a uprobe associated with a uretprobe but not the opposite.
    AddUretprobesFileDescriptors(uretprobes_fds_per_cpu, function);
    AddUprobesFileDescriptors(uprobes_fds_per_cpu, function);

    for (const auto& [cpu, fd] : uretprobes_fds_per_cpu) {
      uprobes_uretpobres_fds_per_cpu[cpu].push_back(fd);
    }
    for (const auto& [cpu, fd] : uprobes_fds_per_cpu) {
      uprobes_uretpobres_fds_per_cpu[cpu].push_back(fd);
    }
  }

  OpenUserSpaceProbesRingBuffers(uprobes_uretpobres_fds_per_cpu);

  return !uprobes_event_open_errors;
}

void TracerThread::OpenUserSpaceProbesRingBuffers(
    const absl::flat_hash_map<int32_t, std::vector<int>>& uprobes_uretpobres_fds_per_cpu) {
  ORBIT_SCOPE_FUNCTION;
  for (const auto& [/*int32_t*/ cpu, /*std::vector<int>*/ fds] : uprobes_uretpobres_fds_per_cpu) {
    if (fds.empty()) continue;

    // Create a single ring buffer per cpu.
    int ring_buffer_fd = fds[0];
    std::string buffer_name = absl::StrFormat("uprobes_uretprobes_%u", cpu);
    ring_buffers_.emplace_back(ring_buffer_fd, UPROBES_RING_BUFFER_SIZE_KB, buffer_name);

    // Redirect subsequent fds to the cpu specific ring buffer created above.
    for (size_t i = 1; i < fds.size(); ++i) {
      perf_event_redirect(fds[i], ring_buffer_fd);
    }
  }
}

bool TracerThread::OpenMmapTask(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  std::vector<int> mmap_task_tracing_fds;
  std::vector<PerfEventRingBuffer> mmap_task_ring_buffers;
  for (int32_t cpu : cpus) {
    int mmap_task_fd = mmap_task_event_open(-1, cpu);
    std::string buffer_name = absl::StrFormat("mmap_task_%d", cpu);
    PerfEventRingBuffer mmap_task_ring_buffer{mmap_task_fd, MMAP_TASK_RING_BUFFER_SIZE_KB,
                                              buffer_name};
    if (mmap_task_ring_buffer.IsOpen()) {
      mmap_task_tracing_fds.push_back(mmap_task_fd);
      mmap_task_ring_buffers.push_back(std::move(mmap_task_ring_buffer));
    } else {
      ERROR("Opening mmap, fork, and exit events for cpu %d", cpu);
      CloseFileDescriptors(mmap_task_tracing_fds);
      return false;
    }
  }

  for (int fd : mmap_task_tracing_fds) {
    tracing_fds_.push_back(fd);
  }
  for (PerfEventRingBuffer& buffer : mmap_task_ring_buffers) {
    ring_buffers_.emplace_back(std::move(buffer));
  }
  return true;
}

bool TracerThread::OpenSampling(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  std::vector<int> sampling_tracing_fds;
  std::vector<PerfEventRingBuffer> sampling_ring_buffers;
  for (int32_t cpu : cpus) {
    int sampling_fd;
    switch (unwinding_method_) {
      case CaptureOptions::kFramePointers:
        sampling_fd = callchain_sample_event_open(sampling_period_ns_, -1, cpu, stack_dump_size_);
        break;
      case CaptureOptions::kDwarf:
        sampling_fd = stack_sample_event_open(sampling_period_ns_, -1, cpu, stack_dump_size_);
        break;
      case CaptureOptions::kUndefined:
      default:
        UNREACHABLE();
        CloseFileDescriptors(sampling_tracing_fds);
        return false;
    }

    std::string buffer_name = absl::StrFormat("sampling_%d", cpu);
    PerfEventRingBuffer sampling_ring_buffer{sampling_fd, SAMPLING_RING_BUFFER_SIZE_KB,
                                             buffer_name};
    if (sampling_ring_buffer.IsOpen()) {
      sampling_tracing_fds.push_back(sampling_fd);
      sampling_ring_buffers.push_back(std::move(sampling_ring_buffer));
    } else {
      ERROR("Opening sampling for cpu %d", cpu);
      CloseFileDescriptors(sampling_tracing_fds);
      return false;
    }
  }

  for (int fd : sampling_tracing_fds) {
    tracing_fds_.push_back(fd);
    uint64_t stream_id = perf_event_get_id(fd);
    if (unwinding_method_ == CaptureOptions::kDwarf) {
      stack_sampling_ids_.insert(stream_id);
    } else if (unwinding_method_ == CaptureOptions::kFramePointers) {
      callchain_sampling_ids_.insert(stream_id);
    }
  }
  for (PerfEventRingBuffer& buffer : sampling_ring_buffers) {
    ring_buffers_.emplace_back(std::move(buffer));
  }
  return true;
}

static void OpenRingBuffersOrRedirectOnExisting(
    const absl::flat_hash_map<int32_t, int>& fds_per_cpu,
    absl::flat_hash_map<int32_t, int>* ring_buffer_fds_per_cpu,
    std::vector<PerfEventRingBuffer>* ring_buffers, uint64_t ring_buffer_size_kb,
    std::string_view buffer_name_prefix) {
  ORBIT_SCOPE_FUNCTION;
  // Redirect all events on the same cpu to a single ring buffer.
  for (const auto& cpu_and_fd : fds_per_cpu) {
    int32_t cpu = cpu_and_fd.first;
    int fd = cpu_and_fd.second;
    if (ring_buffer_fds_per_cpu->contains(cpu)) {
      // Redirect to the already opened ring buffer.
      int ring_bugger_fd = ring_buffer_fds_per_cpu->at(cpu);
      perf_event_redirect(fd, ring_bugger_fd);
    } else {
      // Create a ring buffer for this cpu.
      int ring_buffer_fd = fd;
      std::string buffer_name = absl::StrFormat("%s_%d", buffer_name_prefix, cpu);
      ring_buffers->emplace_back(ring_buffer_fd, ring_buffer_size_kb, buffer_name);
      ring_buffer_fds_per_cpu->emplace(cpu, ring_buffer_fd);
    }
  }
}

namespace {

struct TracepointToOpen {
  TracepointToOpen(const char* tracepoint_category, const char* tracepoint_name,
                   absl::flat_hash_set<uint64_t>* tracepoint_stream_ids)
      : tracepoint_category{tracepoint_category},
        tracepoint_name{tracepoint_name},
        tracepoint_stream_ids{tracepoint_stream_ids} {}

  const char* const tracepoint_category;
  const char* const tracepoint_name;
  absl::flat_hash_set<uint64_t>* const tracepoint_stream_ids;
};

}  // namespace

static bool OpenFileDescriptorsAndRingBuffersForAllTracepoints(
    const std::vector<TracepointToOpen>& tracepoints_to_open, const std::vector<int32_t>& cpus,
    std::vector<int>* tracing_fds, uint64_t ring_buffer_size_kb,
    absl::flat_hash_map<int32_t, int>* tracepoint_ring_buffer_fds_per_cpu_for_redirection,
    std::vector<PerfEventRingBuffer>* ring_buffers) {
  ORBIT_SCOPE_FUNCTION;
  absl::flat_hash_map<size_t, absl::flat_hash_map<int32_t, int>> index_to_tracepoint_fds_per_cpu;
  bool tracepoint_event_open_errors = false;
  for (size_t tracepoint_index = 0;
       tracepoint_index < tracepoints_to_open.size() && !tracepoint_event_open_errors;
       ++tracepoint_index) {
    const char* tracepoint_category = tracepoints_to_open[tracepoint_index].tracepoint_category;
    const char* tracepoint_name = tracepoints_to_open[tracepoint_index].tracepoint_name;
    for (int32_t cpu : cpus) {
      int tracepoint_fd = tracepoint_event_open(tracepoint_category, tracepoint_name, -1, cpu);
      if (tracepoint_fd == -1) {
        ERROR("Opening %s:%s tracepoint for cpu %d", tracepoint_category, tracepoint_name, cpu);
        tracepoint_event_open_errors = true;
        break;
      }
      index_to_tracepoint_fds_per_cpu[tracepoint_index].emplace(cpu, tracepoint_fd);
    }
  }

  if (tracepoint_event_open_errors) {
    for (const auto& index_and_tracepoint_fds_per_cpu : index_to_tracepoint_fds_per_cpu) {
      for (const auto& cpu_and_fd : index_and_tracepoint_fds_per_cpu.second) {
        close(cpu_and_fd.second);
      }
    }
    return false;
  }

  // Since all tracepoints could successfully be opened, we can now commit all file descriptors and
  // ring buffers to TracerThread's members.
  for (const auto& index_and_tracepoint_fds_per_cpu : index_to_tracepoint_fds_per_cpu) {
    const size_t tracepoint_index = index_and_tracepoint_fds_per_cpu.first;
    absl::flat_hash_set<uint64_t>* tracepoint_stream_ids =
        tracepoints_to_open[tracepoint_index].tracepoint_stream_ids;

    for (const auto& cpu_and_fd : index_and_tracepoint_fds_per_cpu.second) {
      tracing_fds->push_back(cpu_and_fd.second);
      tracepoint_stream_ids->insert(perf_event_get_id(cpu_and_fd.second));
    }
  }

  // Redirect on the same ring buffer all the tracepoint events that are open on each CPU.
  for (const auto& index_and_tracepoint_fds_per_cpu : index_to_tracepoint_fds_per_cpu) {
    const size_t tracepoint_index = index_and_tracepoint_fds_per_cpu.first;
    const char* tracepoint_category = tracepoints_to_open[tracepoint_index].tracepoint_category;
    const char* tracepoint_name = tracepoints_to_open[tracepoint_index].tracepoint_name;
    const absl::flat_hash_map<int32_t, int>& tracepoint_fds_per_cpu =
        index_and_tracepoint_fds_per_cpu.second;

    OpenRingBuffersOrRedirectOnExisting(
        tracepoint_fds_per_cpu, tracepoint_ring_buffer_fds_per_cpu_for_redirection, ring_buffers,
        ring_buffer_size_kb, absl::StrFormat("%s:%s", tracepoint_category, tracepoint_name));
  }
  return true;
}

bool TracerThread::OpenThreadNameTracepoints(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  absl::flat_hash_map<int32_t, int> thread_name_tracepoint_ring_buffer_fds_per_cpu;
  return OpenFileDescriptorsAndRingBuffersForAllTracepoints(
      {{"task", "task_newtask", &task_newtask_ids_}, {"task", "task_rename", &task_rename_ids_}},
      cpus, &tracing_fds_, THREAD_NAMES_RING_BUFFER_SIZE_KB,
      &thread_name_tracepoint_ring_buffer_fds_per_cpu, &ring_buffers_);
}

void TracerThread::InitSwitchesStatesNamesVisitor() {
  ORBIT_SCOPE_FUNCTION;
  switches_states_names_visitor_ = std::make_unique<SwitchesStatesNamesVisitor>(listener_);
  switches_states_names_visitor_->SetProduceSchedulingSlices(trace_context_switches_);
  if (trace_thread_state_) {
    switches_states_names_visitor_->SetThreadStatePidFilter(target_pid_);
  }
  switches_states_names_visitor_->SetThreadStateCounter(&stats_.thread_state_count);
  event_processor_.AddVisitor(switches_states_names_visitor_.get());
}

bool TracerThread::OpenContextSwitchAndThreadStateTracepoints(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  std::vector<TracepointToOpen> tracepoints_to_open;
  if (trace_thread_state_ || trace_context_switches_) {
    tracepoints_to_open.emplace_back("sched", "sched_switch", &sched_switch_ids_);
  }
  if (trace_thread_state_) {
    // We also need task:task_newtask, but this is already opened by OpenThreadNameTracepoints.
    tracepoints_to_open.emplace_back("sched", "sched_wakeup", &sched_wakeup_ids_);
  }
  if (tracepoints_to_open.empty()) {
    return true;
  }

  absl::flat_hash_map<int32_t, int> thread_state_tracepoint_ring_buffer_fds_per_cpu;
  return OpenFileDescriptorsAndRingBuffersForAllTracepoints(
      tracepoints_to_open, cpus, &tracing_fds_,
      CONTEXT_SWITCHES_AND_THREAD_STATE_RING_BUFFER_SIZE_KB,
      &thread_state_tracepoint_ring_buffer_fds_per_cpu, &ring_buffers_);
}

void TracerThread::InitGpuTracepointEventVisitor() {
  ORBIT_SCOPE_FUNCTION;
  gpu_event_visitor_ = std::make_unique<GpuTracepointVisitor>(listener_);
  event_processor_.AddVisitor(gpu_event_visitor_.get());
}

// This method enables events for GPU event tracing. We trace three events that correspond to the
// following GPU driver events:
// - A GPU job (command buffer submission) is scheduled by the application. This is tracked by the
//   event "amdgpu_cs_ioctl".
// - A GPU job is scheduled to run on the hardware. This is tracked by the event
//   "amdgpu_sched_run_job".
// - A GPU job is finished by the hardware. This is tracked by the corresponding DMA fence being
//   signaled and is tracked by the event "dma_fence_signaled".
// A single job execution thus corresponds to three events, one of each type above, that share the
// same timeline, context, and seqno.
// We have to record events system-wide (per CPU) to ensure we record all relevant events.
// This method returns true on success, otherwise false.
bool TracerThread::OpenGpuTracepoints(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  absl::flat_hash_map<int32_t, int> gpu_tracepoint_ring_buffer_fds_per_cpu;
  return OpenFileDescriptorsAndRingBuffersForAllTracepoints(
      {{"amdgpu", "amdgpu_cs_ioctl", &amdgpu_cs_ioctl_ids_},
       {"amdgpu", "amdgpu_sched_run_job", &amdgpu_sched_run_job_ids_},
       {"dma_fence", "dma_fence_signaled", &dma_fence_signaled_ids_}},
      cpus, &tracing_fds_, GPU_TRACING_RING_BUFFER_SIZE_KB, &gpu_tracepoint_ring_buffer_fds_per_cpu,
      &ring_buffers_);
}

bool TracerThread::OpenInstrumentedTracepoints(const std::vector<int32_t>& cpus) {
  ORBIT_SCOPE_FUNCTION;
  bool tracepoint_event_open_errors = false;
  absl::flat_hash_map<int32_t, int> tracepoint_ring_buffer_fds_per_cpu;

  for (const auto& selected_tracepoint : instrumented_tracepoints_) {
    absl::flat_hash_set<uint64_t> stream_ids;
    tracepoint_event_open_errors |= !OpenFileDescriptorsAndRingBuffersForAllTracepoints(
        {{selected_tracepoint.category().c_str(), selected_tracepoint.name().c_str(), &stream_ids}},
        cpus, &tracing_fds_, INSTRUMENTED_TRACEPOINTS_RING_BUFFER_SIZE_KB,
        &tracepoint_ring_buffer_fds_per_cpu, &ring_buffers_);

    for (const auto& stream_id : stream_ids) {
      ids_to_tracepoint_info_.emplace(stream_id, selected_tracepoint);
    }
  }

  return !tracepoint_event_open_errors;
}

void TracerThread::InitLostAndDiscardedEventVisitor() {
  ORBIT_SCOPE_FUNCTION;
  lost_and_discarded_event_visitor_ = std::make_unique<LostAndDiscardedEventVisitor>(listener_);
  event_processor_.AddVisitor(lost_and_discarded_event_visitor_.get());
}

static std::vector<ThreadName> RetrieveInitialThreadNamesSystemWide(uint64_t initial_timestamp_ns) {
  std::vector<ThreadName> thread_names;
  for (pid_t pid : GetAllPids()) {
    for (pid_t tid : GetTidsOfProcess(pid)) {
      std::string name = orbit_base::GetThreadName(tid);
      if (name.empty()) {
        continue;
      }

      ThreadName thread_name;
      thread_name.set_pid(pid);
      thread_name.set_tid(tid);
      thread_name.set_name(std::move(name));
      thread_name.set_timestamp_ns(initial_timestamp_ns);
      thread_names.push_back(std::move(thread_name));
    }
  }

  return thread_names;
}

void TracerThread::Startup() {
  ORBIT_SCOPE_FUNCTION;
  Reset();

  // perf_event_open refers to cores as "CPUs".

  // Record context switches from all cores for all processes.
  int32_t number_of_cores = GetNumCores();
  std::vector<int32_t> all_cpus;
  all_cpus.reserve(number_of_cores);
  for (int32_t cpu = 0; cpu < number_of_cores; ++cpu) {
    all_cpus.push_back(cpu);
  }

  // Record calls to dynamically instrumented functions and sample only on cores
  // in this process's cgroup's cpuset, as these are the only cores the process
  // will be scheduled on.
  std::vector<int32_t> cpuset_cpus = GetCpusetCpus(target_pid_);
  if (cpuset_cpus.empty()) {
    ERROR("Could not read cpuset");
    cpuset_cpus = all_cpus;
  }

  // As we open two perf_event_open file descriptors (uprobe and uretprobe) per
  // cpu per instrumented function, increase the maximum number of open files.
  SetMaxOpenFilesSoftLimit(GetMaxOpenFilesHardLimit());

  event_processor_.SetDiscardedOutOfOrderCounter(&stats_.discarded_out_of_order_count);

  InitLostAndDiscardedEventVisitor();

  bool perf_event_open_errors = false;
  std::vector<std::string> perf_event_open_error_details;

  if (bool opened = OpenMmapTask(all_cpus); !opened) {
    perf_event_open_error_details.emplace_back("mmap events, fork and exit events");
    perf_event_open_errors = true;
  }

  if (!instrumented_functions_.empty()) {
    if (bool opened = OpenUserSpaceProbes(cpuset_cpus); !opened) {
      perf_event_open_error_details.emplace_back("u(ret)probes");
      perf_event_open_errors = true;
    }
  }

  // This takes an initial snapshot of the maps. Note that, if at least one
  // function is dynamically instrumented, the snapshot might or might not
  // already contain the [uprobes] map entry. This depends on whether at least
  // one of those functions has already been called after the corresponding
  // uprobes file descriptor has been opened by OpenUserSpaceProbes (opening is
  // enough, it doesn't need to have been enabled).
  InitUprobesEventVisitor();

  if (unwinding_method_ == CaptureOptions::kFramePointers ||
      unwinding_method_ == CaptureOptions::kDwarf) {
    if (bool opened = OpenSampling(cpuset_cpus); !opened) {
      perf_event_open_error_details.emplace_back("sampling");
      perf_event_open_errors = true;
    }
  }

  InitSwitchesStatesNamesVisitor();
  if (bool opened = OpenThreadNameTracepoints(all_cpus); !opened) {
    perf_event_open_error_details.emplace_back(
        "task:task_newtask and task:task_rename tracepoints");
    perf_event_open_errors = true;
  }
  if (trace_context_switches_ || trace_thread_state_) {
    if (bool opened = OpenContextSwitchAndThreadStateTracepoints(all_cpus); !opened) {
      perf_event_open_error_details.emplace_back(
          "sched:sched_switch and sched:sched_wakeup tracepoints");
      perf_event_open_errors = true;
    }
  }

  if (trace_gpu_driver_) {
    // We want to trace all GPU activity, hence we pass 'all_cpus' here.
    if (OpenGpuTracepoints(all_cpus)) {
      InitGpuTracepointEventVisitor();
    } else {
      LOG("There were errors opening GPU tracepoint events");
    }
  }

  if (bool opened = OpenInstrumentedTracepoints(all_cpus); !opened) {
    perf_event_open_error_details.emplace_back("selected tracepoints");
    perf_event_open_errors = true;
  }

  if (perf_event_open_errors) {
    ERROR("With perf_event_open: did you forget to run as root?");
    LOG("In particular, there were errors with opening %s",
        absl::StrJoin(perf_event_open_error_details, ", "));
    orbit_grpc_protos::ErrorsWithPerfEventOpenEvent errors_with_perf_event_open_event;
    errors_with_perf_event_open_event.set_timestamp_ns(orbit_base::CaptureTimestampNs());
    for (std::string& detail : perf_event_open_error_details) {
      errors_with_perf_event_open_event.add_failed_to_open(std::move(detail));
    }
    listener_->OnErrorsWithPerfEventOpenEvent(std::move(errors_with_perf_event_open_event));
  }

  // Start recording events.
  for (int fd : tracing_fds_) {
    perf_event_enable(fd);
  }

  effective_capture_start_timestamp_ns_ = orbit_base::CaptureTimestampNs();

  ModulesSnapshot modules_snapshot;
  modules_snapshot.set_pid(target_pid_);
  modules_snapshot.set_timestamp_ns(effective_capture_start_timestamp_ns_);
  auto modules_or_error = orbit_object_utils::ReadModules(target_pid_);
  if (modules_or_error.has_value()) {
    const std::vector<ModuleInfo>& modules = modules_or_error.value();
    *modules_snapshot.mutable_modules() = {modules.begin(), modules.end()};
    listener_->OnModulesSnapshot(std::move(modules_snapshot));
  } else {
    ERROR("Unable to load modules for %d: %s", target_pid_, modules_or_error.error().message());
  }

  // Get the initial thread names to notify the listener_.
  // All ThreadName events generated by this call will have effective_capture_start_timestamp_ns_ as
  // timestamp. As these events will be the first events of the capture, this prevents later events
  // from having a lower timestamp. After all, the timestamp of the initial ThreadName events is
  // approximate.
  std::vector<ThreadName> thread_names =
      RetrieveInitialThreadNamesSystemWide(effective_capture_start_timestamp_ns_);

  ThreadNamesSnapshot thread_names_snapshot;
  thread_names_snapshot.set_timestamp_ns(effective_capture_start_timestamp_ns_);
  *thread_names_snapshot.mutable_thread_names() = {thread_names.begin(), thread_names.end()};

  listener_->OnThreadNamesSnapshot(std::move(thread_names_snapshot));

  // Get the initial association of tids to pids and pass it to switches_states_names_visitor_.
  RetrieveInitialTidToPidAssociationSystemWide();

  if (trace_thread_state_) {
    // Get the initial thread states and pass them to switches_states_names_visitor_.
    RetrieveInitialThreadStatesOfTarget();
  }

  stats_.Reset();
}

void TracerThread::Shutdown() {
  ORBIT_SCOPE_FUNCTION;
  if (trace_thread_state_) {
    switches_states_names_visitor_->ProcessRemainingOpenStates(orbit_base::CaptureTimestampNs());
  }

  // Stop recording.
  for (int fd : tracing_fds_) {
    perf_event_disable(fd);
  }

  // Close the ring buffers.
  {
    ORBIT_SCOPE("ring_buffers_.clear()");
    ring_buffers_.clear();
  }

  // Close the file descriptors.
  {
    ORBIT_SCOPE_WITH_COLOR(
        absl::StrFormat("Closing %d file descriptors", tracing_fds_.size()).c_str(),
        kOrbitColorRed);
    SCOPED_TIMED_LOG("Closing %d file descriptors", tracing_fds_.size());
    for (int fd : tracing_fds_) {
      ORBIT_SCOPE("Closing fd");
      close(fd);
    }
  }
}

void TracerThread::ProcessOneRecord(PerfEventRingBuffer* ring_buffer) {
  uint64_t event_timestamp_ns = 0;

  perf_event_header header;
  ring_buffer->ReadHeader(&header);

  // perf_event_header::type contains the type of record, e.g.,
  // PERF_RECORD_SAMPLE, PERF_RECORD_MMAP, etc., defined in enum
  // perf_event_type in linux/perf_event.h.
  switch (header.type) {
    case PERF_RECORD_SWITCH:
      ERROR("Unexpected PERF_RECORD_SWITCH in ring buffer '%s'", ring_buffer->GetName());
      break;
    case PERF_RECORD_SWITCH_CPU_WIDE:
      ERROR("Unexpected PERF_RECORD_SWITCH_CPU_WIDE in ring buffer '%s'", ring_buffer->GetName());
      break;
    case PERF_RECORD_FORK:
      event_timestamp_ns = ProcessForkEventAndReturnTimestamp(header, ring_buffer);
      break;
    case PERF_RECORD_EXIT:
      event_timestamp_ns = ProcessExitEventAndReturnTimestamp(header, ring_buffer);
      break;
    case PERF_RECORD_MMAP:
      event_timestamp_ns = ProcessMmapEventAndReturnTimestamp(header, ring_buffer);
      break;
    case PERF_RECORD_SAMPLE:
      event_timestamp_ns = ProcessSampleEventAndReturnTimestamp(header, ring_buffer);
      break;
    case PERF_RECORD_LOST:
      event_timestamp_ns = ProcessLostEventAndReturnTimestamp(header, ring_buffer);
      break;
    case PERF_RECORD_THROTTLE:
    case PERF_RECORD_UNTHROTTLE:
      event_timestamp_ns = ProcessThrottleUnthrottleEventAndReturnTimestamp(header, ring_buffer);
      break;
    default:
      ERROR("Unexpected perf_event_header::type in ring buffer '%s': %u", ring_buffer->GetName(),
            header.type);
      ring_buffer->SkipRecord(header);
      break;
  }

  if (event_timestamp_ns != 0) {
    fds_to_last_timestamp_ns_.insert_or_assign(ring_buffer->GetFileDescriptor(),
                                               event_timestamp_ns);
  }
}

void TracerThread::Run(const std::shared_ptr<std::atomic<bool>>& exit_requested) {
  FAIL_IF(listener_ == nullptr, "No listener set");

  Startup();

  bool last_iteration_saw_events = false;
  std::thread deferred_events_thread(&TracerThread::ProcessDeferredEvents, this);

  while (!(*exit_requested)) {
    ORBIT_SCOPE("TracerThread::Run iteration");

    if (!last_iteration_saw_events) {
      // Periodically print event statistics.
      PrintStatsIfTimerElapsed();

      // Sleep if there was no new event in the last iteration so that we are
      // not constantly polling. Don't sleep so long that ring buffers overflow.
      {
        ORBIT_SCOPE("Sleep");
        usleep(IDLE_TIME_ON_EMPTY_RING_BUFFERS_US);
      }
    }

    last_iteration_saw_events = false;

    // Read and process events from all ring buffers. In order to ensure that no
    // buffer is read constantly while others overflow, we schedule the reading
    // using round-robin like scheduling.
    for (PerfEventRingBuffer& ring_buffer : ring_buffers_) {
      if (*exit_requested) {
        break;
      }

      // Read up to ROUND_ROBIN_POLLING_BATCH_SIZE (5) new events.
      // TODO: Some event types (e.g., stack samples) have a much longer
      //  processing time but are less frequent than others (e.g., context
      //  switches). Take this into account in our scheduling algorithm.
      for (int32_t read_from_this_buffer = 0;
           read_from_this_buffer < ROUND_ROBIN_POLLING_BATCH_SIZE; ++read_from_this_buffer) {
        if (*exit_requested) {
          break;
        }
        if (!ring_buffer.HasNewData()) {
          break;
        }

        last_iteration_saw_events = true;
        ProcessOneRecord(&ring_buffer);
      }
    }
  }

  // Finish processing all deferred events.
  stop_deferred_thread_ = true;
  deferred_events_thread.join();
  event_processor_.ProcessAllEvents();

  Shutdown();
}

uint64_t TracerThread::ProcessForkEventAndReturnTimestamp(const perf_event_header& header,
                                                          PerfEventRingBuffer* ring_buffer) {
  auto event = make_unique_for_overwrite<ForkPerfEvent>();
  ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
  const uint64_t timestamp_ns = event->GetTimestamp();

  if (timestamp_ns < effective_capture_start_timestamp_ns_) {
    return timestamp_ns;
  }

  // PERF_RECORD_FORK is used by SwitchesStatesNamesVisitor
  // to keep the association between tid and pid.
  event->SetOrderedInFileDescriptor(ring_buffer->GetFileDescriptor());
  DeferEvent(std::move(event));

  return timestamp_ns;
}

uint64_t TracerThread::ProcessExitEventAndReturnTimestamp(const perf_event_header& header,
                                                          PerfEventRingBuffer* ring_buffer) {
  auto event = make_unique_for_overwrite<ExitPerfEvent>();
  ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
  const uint64_t timestamp_ns = event->GetTimestamp();

  if (timestamp_ns < effective_capture_start_timestamp_ns_) {
    return timestamp_ns;
  }

  // PERF_RECORD_EXIT is also used by SwitchesStatesNamesVisitor
  // to keep the association between tid and pid.
  event->SetOrderedInFileDescriptor(ring_buffer->GetFileDescriptor());
  DeferEvent(std::move(event));

  return timestamp_ns;
}

uint64_t TracerThread::ProcessMmapEventAndReturnTimestamp(const perf_event_header& header,
                                                          PerfEventRingBuffer* ring_buffer) {
  auto event = ConsumeMmapPerfEvent(ring_buffer, header);
  uint64_t timestamp_ns = event->GetTimestamp();

  if (event->pid() != target_pid_) {
    return timestamp_ns;
  }

  if (event->GetTimestamp() < effective_capture_start_timestamp_ns_) {
    return timestamp_ns;
  }

  event->SetOrderedInFileDescriptor(ring_buffer->GetFileDescriptor());
  DeferEvent(std::move(event));

  return timestamp_ns;
}

uint64_t TracerThread::ProcessSampleEventAndReturnTimestamp(const perf_event_header& header,
                                                            PerfEventRingBuffer* ring_buffer) {
  uint64_t timestamp_ns = ReadSampleRecordTime(ring_buffer);

  if (timestamp_ns < effective_capture_start_timestamp_ns_) {
    // Don't consider events that came before all file descriptors had been enabled.
    ring_buffer->SkipRecord(header);
    return timestamp_ns;
  }

  uint64_t stream_id = ReadSampleRecordStreamId(ring_buffer);
  bool is_uprobe = uprobes_ids_.contains(stream_id);
  bool is_uretprobe = uretprobes_ids_.contains(stream_id);
  bool is_stack_sample = stack_sampling_ids_.contains(stream_id);
  bool is_callchain_sample = callchain_sampling_ids_.contains(stream_id);
  bool is_task_newtask = task_newtask_ids_.contains(stream_id);
  bool is_task_rename = task_rename_ids_.contains(stream_id);
  bool is_sched_switch = sched_switch_ids_.contains(stream_id);
  bool is_sched_wakeup = sched_wakeup_ids_.contains(stream_id);
  bool is_amdgpu_cs_ioctl_event = amdgpu_cs_ioctl_ids_.contains(stream_id);
  bool is_amdgpu_sched_run_job_event = amdgpu_sched_run_job_ids_.contains(stream_id);
  bool is_dma_fence_signaled_event = dma_fence_signaled_ids_.contains(stream_id);
  bool is_user_instrumented_tracepoint = ids_to_tracepoint_info_.contains(stream_id);

  CHECK(is_uprobe + is_uretprobe + is_stack_sample + is_callchain_sample + is_task_newtask +
            is_task_rename + is_sched_switch + is_sched_wakeup + is_amdgpu_cs_ioctl_event +
            is_amdgpu_sched_run_job_event + is_dma_fence_signaled_event +
            is_user_instrumented_tracepoint <=
        1);

  int fd = ring_buffer->GetFileDescriptor();

  if (is_uprobe) {
    auto event = make_unique_for_overwrite<UprobesPerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    using perf_event_uprobe = perf_event_sp_ip_arguments_8bytes_sample;
    constexpr size_t kSizeOfUprobes = sizeof(perf_event_uprobe);
    CHECK(header.size == kSizeOfUprobes);
    if (event->GetPid() != target_pid_) {
      return timestamp_ns;
    }

    event->SetFunction(uprobes_uretprobes_ids_to_function_.at(event->GetStreamId()));
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
    ++stats_.uprobes_count;

  } else if (is_uretprobe) {
    auto event = make_unique_for_overwrite<UretprobesPerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    constexpr size_t kSizeOfUretprobes = sizeof(perf_event_ax_sample);
    CHECK(header.size == kSizeOfUretprobes);
    if (event->GetPid() != target_pid_) {
      return timestamp_ns;
    }

    event->SetFunction(uprobes_uretprobes_ids_to_function_.at(event->GetStreamId()));
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
    ++stats_.uprobes_count;

  } else if (is_stack_sample) {
    pid_t pid = ReadSampleRecordPid(ring_buffer);

    const size_t size_of_stack_sample = sizeof(perf_event_stack_sample_fixed) +
                                        2 * sizeof(uint64_t) /*size and dyn_size*/ +
                                        stack_dump_size_ /*data*/;

    if (header.size != size_of_stack_sample) {
      // Skip stack samples that have an unexpected size. These normally have
      // abi == PERF_SAMPLE_REGS_ABI_NONE and no registers, and size == 0 and
      // no stack. Usually, these samples have pid == tid == 0, but that's not
      // always the case: for example, when a process exits while tracing, we
      // might get a stack sample with pid and tid != 0 but still with
      // abi == PERF_SAMPLE_REGS_ABI_NONE and size == 0.
      ring_buffer->SkipRecord(header);
      return timestamp_ns;
    }
    if (pid != target_pid_) {
      ring_buffer->SkipRecord(header);
      return timestamp_ns;
    }
    // Do *not* filter out samples based on header.misc,
    // e.g., with header.misc == PERF_RECORD_MISC_KERNEL,
    // in general they seem to produce valid callstacks.

    auto event = ConsumeStackSamplePerfEvent(ring_buffer, header);
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
    ++stats_.sample_count;

  } else if (is_callchain_sample) {
    pid_t pid = ReadSampleRecordPid(ring_buffer);
    if (pid != target_pid_) {
      ring_buffer->SkipRecord(header);
      return timestamp_ns;
    }

    auto event = ConsumeCallchainSamplePerfEvent(ring_buffer, header);
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
    ++stats_.sample_count;

  } else if (is_task_newtask) {
    auto event = make_unique_for_overwrite<TaskNewtaskPerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    // task:task_newtask is used by SwitchesStatesNamesVisitor
    // for thread names and thread states.
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
  } else if (is_task_rename) {
    auto event = make_unique_for_overwrite<TaskRenamePerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    // task:task_newtask is used by SwitchesStatesNamesVisitor for thread names.
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));

  } else if (is_sched_switch) {
    auto event = make_unique_for_overwrite<SchedSwitchPerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));
    ++stats_.sched_switch_count;
  } else if (is_sched_wakeup) {
    auto event = make_unique_for_overwrite<SchedWakeupPerfEvent>();
    ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
    event->SetOrderedInFileDescriptor(fd);
    DeferEvent(std::move(event));

  } else if (is_amdgpu_cs_ioctl_event) {
    auto event =
        ConsumeVariableSizeTracepointPerfEvent<AmdgpuCsIoctlPerfEvent>(ring_buffer, header);
    // Do not filter GPU tracepoint events based on pid as we want to have
    // visibility into all GPU activity across the system.
    event->SetOrderedInFileDescriptor(PerfEvent::kNotOrderedInAnyFileDescriptor);
    DeferEvent(std::move(event));
    ++stats_.gpu_events_count;
  } else if (is_amdgpu_sched_run_job_event) {
    auto event =
        ConsumeVariableSizeTracepointPerfEvent<AmdgpuSchedRunJobPerfEvent>(ring_buffer, header);
    event->SetOrderedInFileDescriptor(PerfEvent::kNotOrderedInAnyFileDescriptor);
    DeferEvent(std::move(event));
    ++stats_.gpu_events_count;
  } else if (is_dma_fence_signaled_event) {
    auto event =
        ConsumeVariableSizeTracepointPerfEvent<DmaFenceSignaledPerfEvent>(ring_buffer, header);
    event->SetOrderedInFileDescriptor(PerfEvent::kNotOrderedInAnyFileDescriptor);
    // dma_fence_signaled events can be out of order of timestamp even on the same ring buffer,
    // hence why kNotOrderedInAnyFileDescriptor. To be safe, do the same for the other GPU events.
    DeferEvent(std::move(event));
    ++stats_.gpu_events_count;

  } else if (is_user_instrumented_tracepoint) {
    auto it = ids_to_tracepoint_info_.find(stream_id);

    if (it == ids_to_tracepoint_info_.end()) {
      return timestamp_ns;
    }

    auto event = ConsumeGenericTracepointPerfEvent(ring_buffer, header);

    orbit_grpc_protos::FullTracepointEvent tracepoint_event;
    tracepoint_event.set_pid(event->GetPid());
    tracepoint_event.set_tid(event->GetTid());
    tracepoint_event.set_timestamp_ns(event->GetTimestamp());
    tracepoint_event.set_cpu(event->GetCpu());

    orbit_grpc_protos::TracepointInfo* tracepoint = tracepoint_event.mutable_tracepoint_info();
    tracepoint->set_name(it->second.name());
    tracepoint->set_category(it->second.category());

    listener_->OnTracepointEvent(std::move(tracepoint_event));

  } else {
    ERROR("PERF_EVENT_SAMPLE with unexpected stream_id: %lu", stream_id);
    ring_buffer->SkipRecord(header);
  }

  return timestamp_ns;
}

uint64_t TracerThread::ProcessLostEventAndReturnTimestamp(const perf_event_header& header,
                                                          PerfEventRingBuffer* ring_buffer) {
  auto event = std::make_unique<LostPerfEvent>();
  ring_buffer->ConsumeRecord(header, &event->ring_buffer_record);
  uint64_t timestamp_ns = event->GetTimestamp();

  stats_.lost_count += event->GetNumLost();
  stats_.lost_count_per_buffer[ring_buffer] += event->GetNumLost();

  // Fetch the timestamp of the last event that preceded this PERF_RECORD_LOST in this same ring
  // buffer.
  uint64_t fd_previous_timestamp_ns = 0;
  if (auto it = fds_to_last_timestamp_ns_.find(ring_buffer->GetFileDescriptor());
      it != fds_to_last_timestamp_ns_.end()) {
    fd_previous_timestamp_ns = it->second;
  }
  if (fd_previous_timestamp_ns == 0) {
    // This shouldn't happen because PERF_RECORD_LOST is reported when a ring buffer is full, which
    // means that there were other events in the same ring buffers, and they have already been read.
    ERROR("Unknown previous timestamp for ring buffer '%s'", ring_buffer->GetName());
    return timestamp_ns;
  }

  event->SetPreviousTimestamp(fd_previous_timestamp_ns);
  DeferEvent(std::move(event));

  return timestamp_ns;
}

uint64_t TracerThread::ProcessThrottleUnthrottleEventAndReturnTimestamp(
    const perf_event_header& header, PerfEventRingBuffer* ring_buffer) {
  // Throttle/unthrottle events are reported when sampling causes too much throttling on the CPU.
  // They are usually caused by/reproducible with a very high sampling frequency.
  uint64_t timestamp_ns = ReadThrottleUnthrottleRecordTime(ring_buffer);

  ring_buffer->SkipRecord(header);

  // Simply log throttle/unthrottle events. If they are generated, they are quite low frequency.
  switch (header.type) {
    case PERF_RECORD_THROTTLE:
      LOG("PERF_RECORD_THROTTLE in ring buffer '%s' at timestamp %u", ring_buffer->GetName(),
          timestamp_ns);
      break;
    case PERF_RECORD_UNTHROTTLE:
      LOG("PERF_RECORD_UNTHROTTLE in ring buffer '%s' at timestamp %u", ring_buffer->GetName(),
          timestamp_ns);
      break;
    default:
      UNREACHABLE();
  }

  return timestamp_ns;
}

void TracerThread::DeferEvent(std::unique_ptr<PerfEvent> event) {
  std::lock_guard<std::mutex> lock(deferred_events_mutex_);
  deferred_events_.emplace_back(std::move(event));
}

std::vector<std::unique_ptr<PerfEvent>> TracerThread::ConsumeDeferredEvents() {
  std::lock_guard<std::mutex> lock(deferred_events_mutex_);
  std::vector<std::unique_ptr<PerfEvent>> events(std::move(deferred_events_));
  deferred_events_.clear();
  return events;
}

void TracerThread::ProcessDeferredEvents() {
  pthread_setname_np(pthread_self(), "Proc.Def.Events");
  bool should_exit = false;
  while (!should_exit) {
    ORBIT_SCOPE("ProcessDeferredEvents iteration");
    // When "should_exit" becomes true, we know that we have stopped generating
    // deferred events. The last iteration will consume all remaining events.
    should_exit = stop_deferred_thread_;
    std::vector<std::unique_ptr<PerfEvent>> events = ConsumeDeferredEvents();
    if (events.empty()) {
      // TODO: use a wait/notify mechanism instead of check/sleep.
      ORBIT_SCOPE("Sleep");
      usleep(IDLE_TIME_ON_EMPTY_DEFERRED_EVENTS_US);
    } else {
      {
        ORBIT_SCOPE("AddEvents");
        for (auto& event : events) {
          event_processor_.AddEvent(std::move(event));
        }
      }
      {
        ORBIT_SCOPE("ProcessOldEvents");
        event_processor_.ProcessOldEvents();
      }
    }
  }
}

void TracerThread::RetrieveInitialTidToPidAssociationSystemWide() {
  for (pid_t pid : GetAllPids()) {
    for (pid_t tid : GetTidsOfProcess(pid)) {
      switches_states_names_visitor_->ProcessInitialTidToPidAssociation(tid, pid);
    }
  }
}

void TracerThread::RetrieveInitialThreadStatesOfTarget() {
  for (pid_t tid : GetTidsOfProcess(target_pid_)) {
    uint64_t timestamp_ns = orbit_base::CaptureTimestampNs();
    std::optional<char> state = GetThreadState(tid);
    if (!state.has_value()) {
      continue;
    }
    switches_states_names_visitor_->ProcessInitialState(timestamp_ns, tid, state.value());
  }
}

void TracerThread::Reset() {
  ORBIT_SCOPE_FUNCTION;
  tracing_fds_.clear();
  ring_buffers_.clear();
  fds_to_last_timestamp_ns_.clear();

  uprobes_uretprobes_ids_to_function_.clear();
  uprobes_ids_.clear();
  uretprobes_ids_.clear();
  stack_sampling_ids_.clear();
  callchain_sampling_ids_.clear();
  task_newtask_ids_.clear();
  task_rename_ids_.clear();
  sched_switch_ids_.clear();
  sched_wakeup_ids_.clear();
  amdgpu_cs_ioctl_ids_.clear();
  amdgpu_sched_run_job_ids_.clear();
  dma_fence_signaled_ids_.clear();
  ids_to_tracepoint_info_.clear();

  effective_capture_start_timestamp_ns_ = 0;

  stop_deferred_thread_ = false;
  deferred_events_.clear();
  uprobes_unwinding_visitor_.reset();
  switches_states_names_visitor_.reset();
  gpu_event_visitor_.reset();
  event_processor_.ClearVisitors();
}

void TracerThread::PrintStatsIfTimerElapsed() {
  ORBIT_SCOPE_FUNCTION;
  uint64_t timestamp_ns = orbit_base::CaptureTimestampNs();
  if (stats_.event_count_begin_ns + EVENT_STATS_WINDOW_S * NS_PER_SECOND >= timestamp_ns) {
    return;
  }

  double actual_window_s =
      static_cast<double>(timestamp_ns - stats_.event_count_begin_ns) / NS_PER_SECOND;
  CHECK(actual_window_s > 0.0);

  LOG("Events per second (and total) last %.3f s:", actual_window_s);
  LOG("  sched switches: %.0f/s (%lu)", stats_.sched_switch_count / actual_window_s,
      stats_.sched_switch_count);
  LOG("  samples: %.0f/s (%lu)", stats_.sample_count / actual_window_s, stats_.sample_count);
  LOG("  u(ret)probes: %.0f/s (%lu)", stats_.uprobes_count / actual_window_s, stats_.uprobes_count);
  LOG("  gpu events: %.0f/s (%lu)", stats_.gpu_events_count / actual_window_s,
      stats_.gpu_events_count);

  if (stats_.lost_count_per_buffer.empty()) {
    LOG("  lost: %.0f/s (%lu)", stats_.lost_count / actual_window_s, stats_.lost_count);
  } else {
    LOG("  LOST: %.0f/s (%lu), of which:", stats_.lost_count / actual_window_s, stats_.lost_count);
    for (const auto& buffer_and_lost_count : stats_.lost_count_per_buffer) {
      LOG("    from %s: %.0f/s (%lu)", buffer_and_lost_count.first->GetName().c_str(),
          buffer_and_lost_count.second / actual_window_s, buffer_and_lost_count.second);
    }
  }

  uint64_t discarded_out_of_order_count = stats_.discarded_out_of_order_count;
  LOG("  %s: %.0f/s (%lu)",
      discarded_out_of_order_count == 0 ? "discarded as out of order" : "DISCARDED AS OUT OF ORDER",
      discarded_out_of_order_count / actual_window_s, discarded_out_of_order_count);

  // Ensure we can divide by 0.0 safely in case stats_.sample_count is zero.
  static_assert(std::numeric_limits<double>::is_iec559);

  uint64_t unwind_error_count = stats_.unwind_error_count;
  LOG("  unwind errors: %.0f/s (%lu) [%.1f%%]", unwind_error_count / actual_window_s,
      unwind_error_count, 100.0 * unwind_error_count / stats_.sample_count);
  uint64_t discarded_samples_in_uretprobes_count = stats_.samples_in_uretprobes_count;
  LOG("  samples in u(ret)probes: %.0f/s (%lu) [%.1f%%]",
      discarded_samples_in_uretprobes_count / actual_window_s,
      discarded_samples_in_uretprobes_count,
      100.0 * discarded_samples_in_uretprobes_count / stats_.sample_count);

  uint64_t thread_state_count = stats_.thread_state_count;
  LOG("  target's thread states: %.0f/s (%lu)", thread_state_count / actual_window_s,
      thread_state_count);
  stats_.Reset();
}

}  // namespace orbit_linux_tracing
