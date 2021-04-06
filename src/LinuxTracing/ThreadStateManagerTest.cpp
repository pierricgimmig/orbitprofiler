// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <sys/types.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ThreadStateManager.h"
#include "capture.pb.h"

namespace orbit_linux_tracing {

using orbit_grpc_protos::ThreadStateSlice;

TEST(ThreadStateManager, OneThread) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(100, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);

  slice = manager.OnSchedSwitchOut(300, kPid, kTid, ThreadStateSlice::kInterruptibleSleep);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 300);

  slice = manager.OnSchedWakeup(400, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kInterruptibleSleep);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 400);

  slice = manager.OnSchedSwitchIn(500, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 500);

  std::vector<ThreadStateSlice> slices = manager.OnCaptureFinished(600);
  ASSERT_TRUE(!slices.empty());
  EXPECT_EQ(slices.size(), 1);
  slice = slices[0];
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 600);
}

TEST(ThreadStateManager, NewTask) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnNewTask(100, kPid, kTid);

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);

  slice = manager.OnSchedSwitchOut(300, kPid, kTid, ThreadStateSlice::kRunnable);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 300);

  std::vector<ThreadStateSlice> slices = manager.OnCaptureFinished(400);
  ASSERT_TRUE(!slices.empty());
  EXPECT_EQ(slices.size(), 1);
  slice = slices[0];
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 400);
}

TEST(ThreadStateManager, TwoThreads) {
  constexpr pid_t kPid1 = 41;
  constexpr pid_t kTid1 = 42;
  constexpr pid_t kPid2 = 51;
  constexpr pid_t kTid2 = 52;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(100, kPid1, kTid1, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchIn(200, kPid1, kTid1);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->tid(), kTid1);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);

  manager.OnNewTask(250, kPid2, kTid2);

  slice = manager.OnSchedSwitchOut(300, kPid1, kTid1, ThreadStateSlice::kInterruptibleSleep);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid1);
  EXPECT_EQ(slice->tid(), kTid1);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 300);

  slice = manager.OnSchedSwitchIn(350, kPid2, kTid2);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid2);
  EXPECT_EQ(slice->tid(), kTid2);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 350);

  slice = manager.OnSchedWakeup(400, kPid1, kTid1);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid1);
  EXPECT_EQ(slice->tid(), kTid1);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kInterruptibleSleep);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 400);

  slice = manager.OnSchedSwitchOut(450, kPid2, kTid2, ThreadStateSlice::kRunnable);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid2);
  EXPECT_EQ(slice->tid(), kTid2);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 450);

  slice = manager.OnSchedSwitchIn(500, kPid1, kTid1);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid1);
  EXPECT_EQ(slice->tid(), kTid1);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 500);

  std::vector<ThreadStateSlice> slices = manager.OnCaptureFinished(600);
  ASSERT_TRUE(slices.size() >= 2);
  EXPECT_EQ(slices.size(), 2);

  if (slices[0].tid() > slices[1].tid()) {
    std::swap(slices[0], slices[1]);
  }

  slice = slices[0];
  EXPECT_EQ(slice->pid(), kPid1);
  EXPECT_EQ(slice->tid(), kTid1);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 600);

  slice = slices[1];
  EXPECT_EQ(slice->pid(), kPid2);
  EXPECT_EQ(slice->tid(), kTid2);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 150);
  EXPECT_EQ(slice->end_timestamp_ns(), 600);
}

TEST(ThreadStateManager, SwitchOutAfterInitialStateRunnable) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(100, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchOut(200, kPid, kTid, ThreadStateSlice::kInterruptibleSleep);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, StaleInitialStateWithNewTask) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(150, kPid, kTid, ThreadStateSlice::kRunnable);

  manager.OnNewTask(100, kPid, kTid);

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, StaleInitialStateWithSchedWakeup) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(150, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedWakeup(100, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, StaleInitialStateWithSwitchIn) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(150, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchIn(100, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchOut(200, kPid, kTid, ThreadStateSlice::kRunnable);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, StaleInitialStateWithSwitchOut) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(150, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchOut(100, kPid, kTid, ThreadStateSlice::kInterruptibleSleep);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedWakeup(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kInterruptibleSleep);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, UnknownInitialStateWithSchedWakeup) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  slice = manager.OnSchedWakeup(100, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, UnknownInitialStateWithSwitchIn) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  slice = manager.OnSchedSwitchIn(100, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchOut(200, kPid, kTid, ThreadStateSlice::kRunnable);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, UnknownInitialStateWithSwitchOut) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  slice = manager.OnSchedSwitchOut(100, kPid, kTid, ThreadStateSlice::kInterruptibleSleep);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedWakeup(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kInterruptibleSleep);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, NoStateChangeWithSchedWakeup) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(100, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedWakeup(150, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);
}

TEST(ThreadStateManager, NoStateChangeWithSwitchIn) {
  constexpr pid_t kPid = 41;
  constexpr pid_t kTid = 42;
  ThreadStateManager manager;
  std::optional<ThreadStateSlice> slice;

  manager.OnInitialState(100, kPid, kTid, ThreadStateSlice::kRunnable);

  slice = manager.OnSchedSwitchIn(200, kPid, kTid);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunnable);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 200);

  slice = manager.OnSchedSwitchIn(250, kPid, kTid);
  EXPECT_FALSE(slice.has_value());

  slice = manager.OnSchedSwitchOut(300, kPid, kTid, ThreadStateSlice::kInterruptibleSleep);
  ASSERT_TRUE(slice.has_value());
  EXPECT_EQ(slice->pid(), kPid);
  EXPECT_EQ(slice->tid(), kTid);
  EXPECT_EQ(slice->thread_state(), ThreadStateSlice::kRunning);
  EXPECT_EQ(slice->duration_ns(), 100);
  EXPECT_EQ(slice->end_timestamp_ns(), 300);
}

}  // namespace orbit_linux_tracing
