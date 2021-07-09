// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_MAJOR_PAGEFAULT_TRACK_H_
#define ORBIT_GL_MAJOR_PAGEFAULT_TRACK_H_

#include <string>
#include <utility>

#include "BasicPagefaultTrack.h"

namespace orbit_gl {

class MajorPagefaultTrack final : public BasicPagefaultTrack {
 public:
  explicit MajorPagefaultTrack(Track* parent, TimeGraph* time_graph, orbit_gl::Viewport* viewport,
                               TimeGraphLayout* layout, const std::string& cgroup_name,
                               const orbit_client_model::CaptureData* capture_data,
                               uint32_t indentation_level = 0);

  [[nodiscard]] std::string GetTooltip() const override;

 private:
  [[nodiscard]] std::string GetLegendTooltips(size_t legend_index) const override;
};

}  // namespace orbit_gl

#endif  // ORBIT_GL_MAJOR_PAGEFAULT_TRACK_H_
