// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "AsyncTrack.h"

#include <GteVector.h>
#include <absl/container/flat_hash_map.h>
#include <absl/strings/str_format.h>
#include <absl/time/time.h>

#include <algorithm>

#include "App.h"
#include "Batcher.h"
#include "ClientData/FunctionUtils.h"
#include "ClientData/TextBox.h"
#include "ClientModel/CaptureData.h"
#include "DisplayFormats/DisplayFormats.h"
#include "GlCanvas.h"
#include "GlUtils.h"
#include "Introspection/Introspection.h"
#include "ManualInstrumentationManager.h"
#include "OrbitBase/Logging.h"
#include "TextRenderer.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "TriangleToggle.h"
#include "Viewport.h"
#include "capture_data.pb.h"

using orbit_client_protos::TimerInfo;
using orbit_grpc_protos::InstrumentedFunction;

AsyncTrack::AsyncTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                       orbit_gl::Viewport* viewport, TimeGraphLayout* layout,
                       const std::string& name, OrbitApp* app,
                       const orbit_client_model::CaptureData* capture_data,
                       uint32_t indentation_level)
    : TimerTrack(parent, time_graph, viewport, layout, app, capture_data, indentation_level) {
  SetName(name);
  SetLabel(name);
}

[[nodiscard]] std::string AsyncTrack::GetBoxTooltip(const Batcher& batcher, PickingId id) const {
  const orbit_client_data::TextBox* text_box = batcher.GetTextBox(id);
  if (text_box == nullptr) return "";
  auto* manual_inst_manager = app_->GetManualInstrumentationManager();
  TimerInfo timer_info = text_box->GetTimerInfo();
  orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);

  // The FunctionInfo here corresponds to one of the automatically instrumented empty stubs from
  // Orbit.h. Use it to retrieve the module from which the manually instrumented scope originated.
  const InstrumentedFunction* func =
      capture_data_
          ? capture_data_->GetInstrumentedFunctionById(text_box->GetTimerInfo().function_id())
          : nullptr;
  CHECK(func || timer_info.type() == TimerInfo::kIntrospection ||
        timer_info.type() == TimerInfo::kApiEvent);
  std::string module_name =
      func != nullptr
          ? orbit_client_data::function_utils::GetLoadedModuleNameByPath(func->file_path())
          : "unknown";
  const uint64_t event_id = event.data;
  std::string function_name = manual_inst_manager->GetString(event_id);

  return absl::StrFormat(
      "<b>%s</b><br/>"
      "<i>Timing measured through manual instrumentation</i>"
      "<br/><br/>"
      "<b>Module:</b> %s<br/>"
      "<b>Time:</b> %s",
      function_name, module_name,
      orbit_display_formats::GetDisplayTime(
          TicksToDuration(text_box->GetTimerInfo().start(), text_box->GetTimerInfo().end())));
}

void AsyncTrack::UpdateBoxHeight() {
  box_height_ = layout_->GetTextBoxHeight();
  if (collapse_toggle_->IsCollapsed() && depth_ > 0) {
    box_height_ /= static_cast<float>(depth_);
  }
}

std::vector<std::shared_ptr<orbit_client_data::TimerChain>> AsyncTrack::GetAllSerializableChains()
    const {
  // For async time slices, the start and stop events are their own individual timers and are
  // already serialized on their initial thread tracks. Return an empty vector so that we don't
  // serialize the async timer twice.
  return {};
}

void AsyncTrack::OnTimer(const orbit_client_protos::TimerInfo& timer_info) {
  // Find the first row that that can receive the new timeslice with no overlap.
  // If none of the existing rows works, add a new row.
  uint32_t depth = 0;
  while (max_span_time_by_depth_[depth] > timer_info.start()) ++depth;
  max_span_time_by_depth_[depth] = timer_info.end();

  orbit_client_protos::TimerInfo new_timer_info = timer_info;
  new_timer_info.set_depth(depth);
  TimerTrack::OnTimer(new_timer_info);
}

void AsyncTrack::SetTimesliceText(const TimerInfo& timer_info, float min_x, float z_offset,
                                  orbit_client_data::TextBox* text_box) {
  std::string time = orbit_display_formats::GetDisplayTime(
      absl::Nanoseconds(timer_info.end() - timer_info.start()));
  text_box->SetElapsedTimeTextLength(time.length());

  orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
  const uint64_t event_id = event.data;
  std::string name = app_->GetManualInstrumentationManager()->GetString(event_id);
  std::string text = absl::StrFormat("%s %s", name, time.c_str());
  text_box->SetText(text);

  const Color kTextWhite(255, 255, 255, 255);
  const auto& box_pos = text_box->GetPos();
  const auto& box_size = text_box->GetSize();
  float pos_x = std::max(box_pos.first, min_x);
  float max_size = box_pos.first + box_size.first - pos_x;
  text_renderer_->AddTextTrailingCharsPrioritized(
      text_box->GetText().c_str(), pos_x, box_pos.second + layout_->GetTextOffset(),
      GlCanvas::kZValueBox + z_offset, kTextWhite, text_box->GetElapsedTimeTextLength(),
      layout_->CalculateZoomedFontSize(), max_size);
}

Color AsyncTrack::GetTimerColor(const TimerInfo& timer_info, bool is_selected,
                                bool is_highlighted) const {
  const Color kInactiveColor(100, 100, 100, 255);
  const Color kSelectionColor(0, 128, 255, 255);
  if (is_highlighted) {
    return TimerTrack::kHighlightColor;
  }
  if (is_selected) {
    return kSelectionColor;
  }
  if (!IsTimerActive(timer_info)) {
    return kInactiveColor;
  }

  orbit_api::Event event = ManualInstrumentationManager::ApiEventFromTimerInfo(timer_info);
  const uint64_t event_id = event.data;
  std::string name = app_->GetManualInstrumentationManager()->GetString(event_id);
  Color color = TimeGraph::GetColor(name);

  constexpr uint8_t kOddAlpha = 210;
  if (!(timer_info.depth() & 0x1)) {
    color[3] = kOddAlpha;
  }

  return color;
}
