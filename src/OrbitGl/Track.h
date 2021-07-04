// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_TRACK_H_
#define ORBIT_GL_TRACK_H_

#include <ClientModel/CaptureData.h>
#include <GteVector.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Batcher.h"
#include "BlockChain.h"
#include "CaptureViewElement.h"
#include "CoreMath.h"
#include "OrbitBase/Profiling.h"
#include "TextBox.h"
#include "TextRenderer.h"
#include "TimeGraphLayout.h"
#include "TimerChain.h"
#include "TriangleToggle.h"
#include "Viewport.h"
#include "capture_data.pb.h"

class Track : public orbit_gl::CaptureViewElement, public std::enable_shared_from_this<Track> {
 public:
  enum class Type {
    kTimerTrack,
    kThreadTrack,
    kFrameTrack,
    kVariableTrack,
    kGpuTrack,
    kGraphTrack,
    kSchedulerTrack,
    kAsyncTrack,
    kMemoryTrack,
    kUnknown,
  };

  explicit Track(CaptureViewElement* parent, TimeGraph* time_graph, orbit_gl::Viewport* viewport,
                 TimeGraphLayout* layout, const orbit_client_model::CaptureData* capture_data,
                 uint32_t indentation_level);
  ~Track() override = default;

  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode, float z_offset = 0) override;

  void UpdatePrimitives(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                        PickingMode picking_mode, float z_offset = 0) override;
  void OnDrag(int x, int y) override;

  [[nodiscard]] virtual Type GetType() const = 0;
  [[nodiscard]] virtual bool Movable() { return !pinned_; }

  [[nodiscard]] virtual float GetHeight() const = 0;
  [[nodiscard]] bool GetVisible() const { return visible_; }
  void SetVisible(bool value) { visible_ = value; }

  void SetColor(const Color& color) { color_ = color; }

  [[nodiscard]] uint32_t GetNumTimers() const { return num_timers_; }
  [[nodiscard]] virtual uint64_t GetMinTime() const { return min_time_; }
  [[nodiscard]] virtual uint64_t GetMaxTime() const { return max_time_; }
  void SetNumberOfPrioritizedTrailingCharacters(int num_characters) {
    num_prioritized_trailing_characters_ = num_characters;
  }
  [[nodiscard]] int GetNumberOfPrioritizedTrailingCharacters() const {
    return num_prioritized_trailing_characters_;
  }

  virtual void OnTimer(const orbit_client_protos::TimerInfo& /*timer_info*/) {}
  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetTimers() const { return {}; }
  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetAllChains() const = 0;
  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetAllSerializableChains()
      const = 0;
  [[nodiscard]] bool IsPinned() const { return pinned_; }
  void SetPinned(bool value);

  [[nodiscard]] bool IsMoving() const { return picked_ && mouse_pos_last_click_ != mouse_pos_cur_; }
  void SetName(const std::string& name) { name_ = name; }
  [[nodiscard]] const std::string& GetName() const { return name_; }
  void SetLabel(const std::string& label) { label_ = label; }
  [[nodiscard]] const std::string& GetLabel() const { return label_; }

  [[nodiscard]] virtual Color GetTrackBackgroundColor() const;

  virtual void OnCollapseToggle(bool is_collapsed);
  [[nodiscard]] virtual bool IsCollapsible() const { return false; }
  [[nodiscard]] virtual bool IsFilterable() const { return !pinned_; }
  TriangleToggle* GetTriangleToggle() const { return collapse_toggle_.get(); }
  [[nodiscard]] int32_t GetProcessId() const { return process_id_; }
  void SetProcessId(uint32_t pid) { process_id_ = pid; }
  [[nodiscard]] virtual bool IsEmpty() const = 0;

  [[nodiscard]] virtual bool IsTrackSelected() const { return false; }

  [[nodiscard]] bool IsCollapsed() const { return collapse_toggle_->IsCollapsed(); }

  [[nodiscard]] virtual std::vector<CaptureViewElement*> GetVisibleChildren() { return {}; }
  [[nodiscard]] virtual int GetVisiblePrimitiveCount() const { return 0; }

  [[nodiscard]] virtual uint32_t GetIntent() const { return indentation_level_; }

 protected:
  // Returns the y-position of the triangle.
  float DrawCollapsingTriangle(Batcher& batcher, TextRenderer& text_renderer,
                               uint64_t current_mouse_time_ns, PickingMode picking_mode,
                               float z_offset = 0);
  void DrawTriangleFan(Batcher& batcher, const std::vector<Vec2>& points, const Vec2& pos,
                       const Color& color, float rotation, float z);

  std::unique_ptr<orbit_accessibility::AccessibleInterface> CreateAccessibleInterface() override;

  std::string name_;
  std::string label_;
  int num_prioritized_trailing_characters_;
  int32_t thread_id_;
  int32_t process_id_;
  Color color_;
  bool draw_background_ = true;
  bool visible_ = true;
  bool pinned_ = false;
  std::map<int, std::shared_ptr<TimerChain>> timers_;
  std::atomic<uint32_t> num_timers_;
  std::atomic<uint64_t> min_time_;
  std::atomic<uint64_t> max_time_;
  Type type_ = Type::kUnknown;
  std::shared_ptr<TriangleToggle> collapse_toggle_;

  TimeGraphLayout* layout_;

  const orbit_client_model::CaptureData* capture_data_ = nullptr;

 private:
  const uint32_t indentation_level_;
};

#endif
