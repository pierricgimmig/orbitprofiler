// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LINUX_TRACING_PERF_EVENT_RECORDS_H_
#define LINUX_TRACING_PERF_EVENT_RECORDS_H_

#include "PerfEventOpen.h"

namespace orbit_linux_tracing {

// This struct must be in sync with the SAMPLE_TYPE_TID_TIME_STREAMID_CPU in
// PerfEventOpen.h, as the bits set in perf_event_attr::sample_type determine
// the fields this struct should have.
struct __attribute__((__packed__)) perf_event_sample_id_tid_time_streamid_cpu {
  uint32_t pid, tid;  /* if PERF_SAMPLE_TID */
  uint64_t time;      /* if PERF_SAMPLE_TIME */
  uint64_t stream_id; /* if PERF_SAMPLE_STREAM_ID */
  uint32_t cpu, res;  /* if PERF_SAMPLE_CPU */
};

struct __attribute__((__packed__)) perf_event_context_switch {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

struct __attribute__((__packed__)) perf_event_context_switch_cpu_wide {
  perf_event_header header;
  uint32_t next_prev_pid;
  uint32_t next_prev_tid;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

struct __attribute__((__packed__)) perf_event_fork_exit {
  perf_event_header header;
  uint32_t pid, ppid;
  uint32_t tid, ptid;
  uint64_t time;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

// This struct must be in sync with the SAMPLE_REGS_USER_ALL in
// PerfEventOpen.h.
struct __attribute__((__packed__)) perf_event_sample_regs_user_all {
  uint64_t abi;
  uint64_t ax;
  uint64_t bx;
  uint64_t cx;
  uint64_t dx;
  uint64_t si;
  uint64_t di;
  uint64_t bp;
  uint64_t sp;
  uint64_t ip;
  uint64_t flags;
  uint64_t cs;
  uint64_t ss;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
};

// This struct must be in sync with the SAMPLE_REGS_USER_AX in
// PerfEventOpen.h.
struct __attribute__((__packed__)) perf_event_sample_regs_user_ax {
  uint64_t abi;
  uint64_t ax;
};

// This struct must be in sync with the SAMPLE_REGS_USER_SP_IP_ARGUMENTS in
// PerfEventOpen.h.
struct __attribute__((__packed__)) perf_event_sample_regs_user_sp_ip_arguments {
  uint64_t abi;
  uint64_t cx;
  uint64_t dx;
  uint64_t si;
  uint64_t di;
  uint64_t sp;
  uint64_t ip;
  uint64_t r8;
  uint64_t r9;
};

struct __attribute__((__packed__)) perf_event_sample_stack_user_8bytes {
  uint64_t size;
  uint64_t top8bytes;
  uint64_t dyn_size;
};

struct __attribute__((__packed__)) perf_event_stack_sample_fixed {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  perf_event_sample_regs_user_all regs;
  // Following this field there are the following fields, which we read dynamically:
  // uint64_t size;                     /* if PERF_SAMPLE_STACK_USER */
  // char data[SAMPLE_STACK_USER_SIZE]; /* if PERF_SAMPLE_STACK_USER */
  // uint64_t dyn_size;                 /* if PERF_SAMPLE_STACK_USER && size != 0 */
};

struct __attribute__((__packed__)) perf_event_callchain_sample_fixed {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  uint64_t nr;
  // Following this field there are the following fields, which we read dynamically:
  // uint64_t[nr] ips;
  // perf_event_sample_regs_user_sp_ip_arguments regs;
  // uint64_t size;
  // char data[size];
  // uint64_t dyn_size;
};

struct __attribute__((__packed__)) perf_event_sp_ip_arguments_8bytes_sample {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  perf_event_sample_regs_user_sp_ip_arguments regs;
  perf_event_sample_stack_user_8bytes stack;
};

struct __attribute__((__packed__)) perf_event_ax_sample {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  perf_event_sample_regs_user_ax regs;
};

template <typename TracepointT>
struct __attribute__((__packed__)) perf_event_raw_sample {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  uint32_t size;
  TracepointT data;
};

struct __attribute__((__packed__)) perf_event_raw_sample_fixed {
  perf_event_header header;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  uint32_t size;
  // The rest of the sample is a char[size] that we read dynamically.
};

struct __attribute__((__packed__)) perf_event_mmap_up_to_pgoff {
  perf_event_header header;
  uint32_t pid;
  uint32_t tid;
  uint64_t address;
  uint64_t length;
  uint64_t page_offset;
  // OMITTED: char filename[]
  // OMITTED: perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

struct __attribute__((__packed__)) perf_event_lost {
  perf_event_header header;
  uint64_t id;
  uint64_t lost;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

struct __attribute__((__packed__)) perf_event_throttle_unthrottle {
  perf_event_header header;
  uint64_t time;
  uint64_t id;
  uint64_t lost;
  perf_event_sample_id_tid_time_streamid_cpu sample_id;
};

}  // namespace orbit_linux_tracing

#endif  // LINUX_TRACING_PERF_EVENT_RECORDS_H_
