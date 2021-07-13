// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LINUX_TRACING_TRACEPOINTS_H_
#define LINUX_TRACING_TRACEPOINTS_H_

#include <cstdint>

// Format is based on the content of the event's format file:
// /sys/kernel/debug/tracing/events/<category>/<name>/format

struct __attribute__((__packed__)) tracepoint_common {
  uint16_t common_type;
  uint8_t common_flags;
  uint8_t common_preempt_count;
  int32_t common_pid;
};

struct __attribute__((__packed__)) task_newtask_tracepoint {
  tracepoint_common common;
  int32_t pid;
  char comm[16];
  uint64_t clone_flags;
  int16_t oom_score_adj;
  char reserved[14];  // These bytes are not documented in the format file.
};

static_assert(sizeof(task_newtask_tracepoint) == 52);

struct __attribute__((__packed__)) task_rename_tracepoint {
  tracepoint_common common;
  int32_t pid;
  char oldcomm[16];
  char newcomm[16];
  int16_t oom_score_adj;
  char reserved[6];  // These bytes are not documented in the format file.
};

static_assert(sizeof(task_rename_tracepoint) == 52);

struct __attribute__((__packed__)) sched_switch_tracepoint {
  tracepoint_common common;
  char prev_comm[16];
  int32_t prev_pid;
  int32_t prev_prio;
  int64_t prev_state;
  char next_comm[16];
  int32_t next_pid;
  int32_t next_prio;
  char reserved[4];  // These bytes are not documented in the format file.
};

static_assert(sizeof(sched_switch_tracepoint) == 68);

struct __attribute__((__packed__)) sched_wakeup_tracepoint {
  tracepoint_common common;
  char comm[16];
  int32_t pid;
  int32_t prio;
  int32_t success;
  int32_t target_cpu;
  char reserved[4];  // These bytes are not documented in the format file.
};

static_assert(sizeof(sched_wakeup_tracepoint) == 44);

struct __attribute__((__packed__)) amdgpu_cs_ioctl_tracepoint {
  tracepoint_common common;
  uint64_t sched_job_id;
  int32_t timeline;
  uint32_t context;
  uint32_t seqno;
  uint64_t dma_fence;  // This is an address.
  uint64_t ring_name;  // This is an address.
  uint32_t num_ibs;
};

struct __attribute__((__packed__)) amdgpu_sched_run_job_tracepoint {
  tracepoint_common common;
  uint64_t sched_job_id;
  int32_t timeline;
  uint32_t context;
  uint32_t seqno;
  uint64_t ring_name;  // This is an address.
  uint32_t num_ibs;
};

struct __attribute__((__packed__)) dma_fence_signaled_tracepoint {
  tracepoint_common common;
  int32_t driver;
  int32_t timeline;
  uint32_t context;
  uint32_t seqno;
};

#endif  // LINUX_TRACING_TRACEPOINTS_H_
