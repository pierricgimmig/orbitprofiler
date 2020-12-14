// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "GpuTracepointEventProcessor.h"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <ctime>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "capture.pb.h"

namespace LinuxTracing {

using orbit_grpc_protos::GpuJob;

int GpuTracepointEventProcessor::ComputeDepthForEvent(const std::string& timeline,
                                                      uint64_t start_timestamp,
                                                      uint64_t end_timestamp) {
  if (!timeline_to_latest_timestamp_per_depth_.contains(timeline)) {
    timeline_to_latest_timestamp_per_depth_.emplace(timeline, std::vector<uint64_t>{});
  }
  auto it = timeline_to_latest_timestamp_per_depth_.find(timeline);
  std::vector<uint64_t>& latest_timestamps_per_depth = it->second;

  for (size_t depth = 0; depth < latest_timestamps_per_depth.size(); ++depth) {
    // We add a small amount of slack on each row of the GPU track timeline to
    // make sure events don't get too crowded.
    static constexpr uint64_t kSlackNs = 1'000'000;
    if (start_timestamp >= (latest_timestamps_per_depth[depth] + kSlackNs)) {
      latest_timestamps_per_depth[depth] = end_timestamp;
      return depth;
    }
  }

  // Note that this vector only grows in size until a certain maximum depth is
  // reached. Since there are only O(10) events per frame created, the depth
  // is not likely to grow to a very large size.
  latest_timestamps_per_depth.push_back(end_timestamp);
  return static_cast<int>(latest_timestamps_per_depth.size() - 1);
}

void GpuTracepointEventProcessor::CreateGpuExecutionEventIfComplete(const Key& key) {
  auto cs_it = amdgpu_cs_ioctl_events_.find(key);
  auto sched_it = amdgpu_sched_run_job_events_.find(key);
  auto dma_it = dma_fence_signaled_events_.find(key);

  // First check if we have received all three events that are needed
  // to complete a full GPU execution event. Otherwise, we need to
  // keep waiting for events for this context, seqno, and timeline.
  if (cs_it == amdgpu_cs_ioctl_events_.end() || sched_it == amdgpu_sched_run_job_events_.end() ||
      dma_it == dma_fence_signaled_events_.end()) {
    return;
  }

  std::string timeline = cs_it->second.timeline;
  pid_t tid = cs_it->second.tid;

  // We assume that GPU jobs (command buffer submissions) immediately
  // start running on the hardware when they are scheduled by the
  // driver (this is the best we can do), *unless* there is already a
  // job running. We keep track of when jobs finish in
  // timeline_to_latest_dma_signal_. If a previous job is still running
  // at the timestamp of scheduling the current job, we push the start
  // time for starting on the hardware back.
  if (!timeline_to_latest_dma_signal_.contains(timeline)) {
    // When there is not yet an entry in timeline_to_latest_dma_signal_ for the current timeline,
    // this means that no previous GPU job has been executed on this timeline during our capture.
    // We just have to set a timestamp here that precedes any event on the timeline to make sure
    // that hw_start_time below is set correctly, hence why we use "0".
    timeline_to_latest_dma_signal_.emplace(timeline, 0);
  }
  auto latest_dma_it = timeline_to_latest_dma_signal_.find(timeline);
  // We do not have an explicit event for the following timestamp. We
  // assume that, when the GPU queue corresponding to timeline is
  // not executing a job, that this job starts exactly when it is
  // scheduled by the driver. Otherwise, we assume it starts exactly
  // when the previous job has signaled that it is done. Since we do
  // not have an explicit signal here, this is the best we can do.
  uint64_t hw_start_time = sched_it->second.timestamp_ns;
  if (hw_start_time < latest_dma_it->second) {
    hw_start_time = latest_dma_it->second;
  }

  int depth =
      ComputeDepthForEvent(timeline, cs_it->second.timestamp_ns, dma_it->second.timestamp_ns);
  GpuJob gpu_job;
  gpu_job.set_tid(tid);
  gpu_job.set_context(cs_it->second.context);
  gpu_job.set_seqno(cs_it->second.seqno);
  gpu_job.set_timeline(cs_it->second.timeline);
  gpu_job.set_depth(depth);
  gpu_job.set_amdgpu_cs_ioctl_time_ns(cs_it->second.timestamp_ns);
  gpu_job.set_amdgpu_sched_run_job_time_ns(sched_it->second.timestamp_ns);
  gpu_job.set_gpu_hardware_start_time_ns(hw_start_time);
  gpu_job.set_dma_fence_signaled_time_ns(dma_it->second.timestamp_ns);

  CHECK(listener_ != nullptr);
  listener_->OnGpuJob(std::move(gpu_job));

  // We need to update the timestamp when the last GPU job so far seen
  // finishes on this timeline.
  latest_dma_it->second = std::max(latest_dma_it->second, dma_it->second.timestamp_ns);

  amdgpu_cs_ioctl_events_.erase(key);
  amdgpu_sched_run_job_events_.erase(key);
  dma_fence_signaled_events_.erase(key);
}

// The following three overloaded PushEvent methods handle the three different
// types of events that we can get from the GPU driver tracepoints we are
// tracing.
// We allow for the possibility that these events arrive out-of-order (depending
// on the order in which events are read from perf_event_open ring buffers) with
// the following approach: We record all three types of events in different
// maps. Whenever a new event arrives, we add it to the corresponding map and
// then try to create a complete GPU execution event. This event is only created
// when all three types of GPU events have been received.

void GpuTracepointEventProcessor::PushEvent(const AmdgpuCsIoctlPerfEvent& sample) {
  AmdgpuCsIoctlEvent event{sample.GetTid(), sample.GetTimestamp(), sample.GetContext(),
                           sample.GetSeqno(), sample.ExtractTimelineString()};
  Key key = std::make_tuple(sample.GetContext(), sample.GetSeqno(), sample.ExtractTimelineString());

  amdgpu_cs_ioctl_events_.emplace(key, std::move(event));
  CreateGpuExecutionEventIfComplete(key);
}

void GpuTracepointEventProcessor::PushEvent(const AmdgpuSchedRunJobPerfEvent& sample) {
  AmdgpuSchedRunJobEvent event{sample.GetTimestamp(), sample.GetContext(), sample.GetSeqno(),
                               sample.ExtractTimelineString()};
  Key key = std::make_tuple(sample.GetContext(), sample.GetSeqno(), sample.ExtractTimelineString());

  amdgpu_sched_run_job_events_.emplace(key, std::move(event));
  CreateGpuExecutionEventIfComplete(key);
}

void GpuTracepointEventProcessor::PushEvent(const DmaFenceSignaledPerfEvent& sample) {
  DmaFenceSignaledEvent event{sample.GetTimestamp(), sample.GetContext(), sample.GetSeqno(),
                              sample.ExtractTimelineString()};
  Key key = std::make_tuple(sample.GetContext(), sample.GetSeqno(), sample.ExtractTimelineString());

  dma_fence_signaled_events_.emplace(key, std::move(event));
  CreateGpuExecutionEventIfComplete(key);
}

void GpuTracepointEventProcessor::SetListener(TracerListener* listener) { listener_ = listener; }

}  // namespace LinuxTracing
