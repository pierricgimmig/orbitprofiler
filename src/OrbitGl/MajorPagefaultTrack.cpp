// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "MajorPagefaultTrack.h"

#include <absl/strings/substitute.h>

namespace orbit_gl {
MajorPagefaultTrack::MajorPagefaultTrack(Track* parent, TimeGraph* time_graph,
                                         orbit_gl::Viewport* viewport, TimeGraphLayout* layout,
                                         const std::string& cgroup_name,
                                         uint64_t memory_sampling_period_ms,
                                         const orbit_client_model::CaptureData* capture_data,
                                         uint32_t indentation_level)
    : BasicPagefaultTrack(parent, time_graph, viewport, layout, "Major Pagefault Track",
                          cgroup_name, memory_sampling_period_ms, capture_data, indentation_level) {
  index_of_series_to_highlight_ = static_cast<size_t>(SeriesIndex::kProcess);
}

std::string MajorPagefaultTrack::GetTooltip() const {
  return "Shows major pagefault statistics. A major pagefault occurs when the requested page does "
         "not reside in the main memory or CPU cache, and has to be swapped from an external "
         "storage.<br/> The major pagefaults might cause slow performance of the target process.";
}

std::string MajorPagefaultTrack::GetLegendTooltips(size_t legend_index) const {
  switch (static_cast<SeriesIndex>(legend_index)) {
    case SeriesIndex::kProcess:
      return absl::Substitute(
          "<b>Number of major pagefaults incurred by the $0 process during the sampling "
          "period ($2 ms).</b><br/><br/>"
          "Derived from the <i>majflt</i> field in file <i>/proc/$1/stat</i>.",
          capture_data_->process_name(), capture_data_->process_id(), memory_sampling_period_ms_);
    case SeriesIndex::kCGroup:
      return absl::Substitute(
          "<b>Number of major pagefaults incurred by the $0 cgroup during the sampling "
          "period ($1 ms).</b><br/><br/>"
          "Derived from the <i>pgmajfault</i> field in file "
          "<i>/sys/fs/cgroup/memory/$0/memory.stat</i>.",
          cgroup_name_, memory_sampling_period_ms_);
    case SeriesIndex::kSystem:
      return absl::Substitute(
          "<b>Number of system-wide major pagefaults occurred during the sampling "
          "period ($0 ms).</b><br/><br/>"
          "Derived from the <i>pgmajfault</i> field in file <i>/proc/vmstat</i>.",
          memory_sampling_period_ms_);
    default:
      UNREACHABLE();
  }
}

}  // namespace orbit_gl
