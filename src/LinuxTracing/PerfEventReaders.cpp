// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "PerfEventReaders.h"

#include <string>
#include <utility>
#include <vector>

#include "OrbitBase/Logging.h"
#include "PerfEventRecords.h"
#include "PerfEventRingBuffer.h"

namespace orbit_linux_tracing {

void ReadPerfSampleIdAll(PerfEventRingBuffer* ring_buffer, const perf_event_header& header,
                         perf_event_sample_id_tid_time_streamid_cpu* sample_id) {
  CHECK(sample_id != nullptr);
  CHECK(header.size >
        sizeof(perf_event_header) + sizeof(perf_event_sample_id_tid_time_streamid_cpu));
  // sample_id_all is always the last field in the event
  uint64_t offset = header.size - sizeof(perf_event_sample_id_tid_time_streamid_cpu);
  ring_buffer->ReadValueAtOffset(sample_id, offset);
}

uint64_t ReadSampleRecordTime(PerfEventRingBuffer* ring_buffer) {
  uint64_t time;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &time,
      sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, time));
  return time;
}

uint64_t ReadSampleRecordStreamId(PerfEventRingBuffer* ring_buffer) {
  uint64_t stream_id;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &stream_id,
      sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, stream_id));
  return stream_id;
}

pid_t ReadSampleRecordPid(PerfEventRingBuffer* ring_buffer) {
  pid_t pid;
  // All PERF_RECORD_SAMPLEs start with
  //   perf_event_header header;
  //   perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ring_buffer->ReadValueAtOffset(
      &pid, sizeof(perf_event_header) + offsetof(perf_event_sample_id_tid_time_streamid_cpu, pid));
  return pid;
}

uint64_t ReadThrottleUnthrottleRecordTime(PerfEventRingBuffer* ring_buffer) {
  // Note that perf_event_throttle_unthrottle::time and
  // perf_event_sample_id_tid_time_streamid_cpu::time differ a bit. Use the latter as we use that
  // for all other events.
  uint64_t time;
  ring_buffer->ReadValueAtOffset(&time,
                                 offsetof(perf_event_throttle_unthrottle, sample_id) +
                                     offsetof(perf_event_sample_id_tid_time_streamid_cpu, time));
  return time;
}

std::unique_ptr<MmapPerfEvent> ConsumeMmapPerfEvent(PerfEventRingBuffer* ring_buffer,
                                                    const perf_event_header& header) {
  // Mmap records have the following layout:
  // struct {
  //   struct perf_event_header header;
  //   u32    pid, tid;
  //   u64    addr;
  //   u64    len;
  //   u64    pgoff;
  //   char   filename[];
  //   struct sample_id sample_id; /* if sample_id_all */
  // };
  // Because of filename, the layout is not fixed.

  perf_event_sample_id_tid_time_streamid_cpu sample_id;
  ReadPerfSampleIdAll(ring_buffer, header, &sample_id);

  perf_event_mmap_up_to_pgoff mmap_event;
  ring_buffer->ReadValueAtOffset(&mmap_event, 0);

  // read filename
  size_t filename_offset = sizeof(perf_event_mmap_up_to_pgoff);
  // strictly > because filename is null-terminated string
  CHECK(header.size > (filename_offset + sizeof(perf_event_sample_id_tid_time_streamid_cpu)));
  size_t filename_size =
      header.size - filename_offset - sizeof(perf_event_sample_id_tid_time_streamid_cpu);
  std::vector<char> filename_vector(filename_size);
  ring_buffer->ReadRawAtOffset(&filename_vector[0], filename_offset, filename_size);
  // This is a bit paranoid but you never know
  filename_vector[filename_size - 1] = '\0';
  std::string filename(filename_vector.data());

  ring_buffer->SkipRecord(header);

  // Workaround for gcc's "cannot bind packed field ... to ‘long unsigned int&’"
  uint64_t timestamp = sample_id.time;
  int32_t pid = static_cast<int32_t>(sample_id.pid);

  // Consider moving this to MMAP2 event which has more information (like flags)
  return std::make_unique<MmapPerfEvent>(pid, timestamp, mmap_event, std::move(filename));
}

std::unique_ptr<StackSamplePerfEvent> ConsumeStackSamplePerfEvent(PerfEventRingBuffer* ring_buffer,
                                                                  const perf_event_header& header) {
  // We expect the following layout of the perf event:
  //  struct {
  //    struct perf_event_header header;
  //    u64 sample_id;          /* if PERF_SAMPLE_IDENTIFIER */
  //    u32 pid, tid;           /* if PERF_SAMPLE_TID */
  //    u64 time;               /* if PERF_SAMPLE_TIME */
  //    u64 stream_id;          /* if PERF_SAMPLE_STREAM_ID */
  //    u32 cpu, res;           /* if PERF_SAMPLE_CPU */
  //    u64 abi;                /* if PERF_SAMPLE_REGS_USER */
  //    u64 regs[weight(mask)]; /* if PERF_SAMPLE_REGS_USER */
  //    u64 size;               /* if PERF_SAMPLE_STACK_USER */
  //    char data[size];        /* if PERF_SAMPLE_STACK_USER */
  //    u64 dyn_size;           /* if PERF_SAMPLE_STACK_USER && size != 0 */
  //  };
  // Unfortunately, the value of `size` is not constant, so we need to compute the offsets by hand,
  // rather than relying on a struct.

  size_t offset_of_size =
      offsetof(perf_event_stack_sample_fixed, regs) + sizeof(perf_event_sample_regs_user_all);
  size_t offset_of_data = offset_of_size + sizeof(uint64_t);

  uint64_t size = 0;
  ring_buffer->ReadValueAtOffset(&size, offset_of_size);

  size_t offset_of_dyn_size = offset_of_data + (size * sizeof(char));

  uint64_t dyn_size = 0;
  ring_buffer->ReadValueAtOffset(&dyn_size, offset_of_dyn_size);

  auto event = std::make_unique<StackSamplePerfEvent>(dyn_size);
  event->ring_buffer_record.header = header;
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record.sample_id,
                                 offsetof(perf_event_stack_sample_fixed, sample_id));
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record.regs,
                                 offsetof(perf_event_stack_sample_fixed, regs));
  ring_buffer->ReadRawAtOffset(event->ring_buffer_record.stack.data.get(), offset_of_data,
                               dyn_size);
  ring_buffer->SkipRecord(header);
  return event;
}

std::unique_ptr<CallchainSamplePerfEvent> ConsumeCallchainSamplePerfEvent(
    PerfEventRingBuffer* ring_buffer, const perf_event_header& header) {
  uint64_t nr = 0;
  ring_buffer->ReadValueAtOffset(&nr, offsetof(perf_event_callchain_sample_fixed, nr));

  uint64_t size_of_ips_in_bytes = nr * sizeof(uint64_t) / sizeof(char);

  // We expect the following layout of the perf event:
  //  struct {
  //    struct perf_event_header header;
  //    u64 sample_id;          /* if PERF_SAMPLE_IDENTIFIER */
  //    u32 pid, tid;           /* if PERF_SAMPLE_TID */
  //    u64 time;               /* if PERF_SAMPLE_TIME */
  //    u64 stream_id;          /* if PERF_SAMPLE_STREAM_ID */
  //    u32 cpu, res;           /* if PERF_SAMPLE_CPU */
  //    u64 nr;                 /* if PERF_SAMPLE_CALLCHAIN */
  //    u64 ips[nr];            /* if PERF_SAMPLE_CALLCHAIN */
  //    u64 abi;                /* if PERF_SAMPLE_REGS_USER */
  //    u64 regs[weight(mask)]; /* if PERF_SAMPLE_REGS_USER */
  //    u64 size;               /* if PERF_SAMPLE_STACK_USER */
  //    char data[size];        /* if PERF_SAMPLE_STACK_USER */
  //    u64 dyn_size;           /* if PERF_SAMPLE_STACK_USER && size != 0 */
  //  };
  // Unfortunately, the number of `ips` is dynamic, so we need to compute the offsets by hand,
  // rather than relying on a struct.

  size_t offset_of_ips = offsetof(perf_event_callchain_sample_fixed, nr) +
                         sizeof(perf_event_callchain_sample_fixed::nr);
  size_t offset_of_regs_user_struct = offset_of_ips + size_of_ips_in_bytes;
  size_t offset_of_size = offset_of_regs_user_struct + sizeof(perf_event_sample_regs_user_all);
  size_t offset_of_data = offset_of_size + sizeof(uint64_t);

  uint64_t size = 0;
  ring_buffer->ReadRawAtOffset(&size, offset_of_size, sizeof(uint64_t));

  size_t offset_of_dyn_size = offset_of_data + (size * sizeof(char));

  uint64_t dyn_size = 0;
  ring_buffer->ReadRawAtOffset(&dyn_size, offset_of_dyn_size, sizeof(uint64_t));
  auto event = std::make_unique<CallchainSamplePerfEvent>(nr, dyn_size);
  event->ring_buffer_record.header = header;
  ring_buffer->ReadValueAtOffset(&event->ring_buffer_record.sample_id,
                                 offsetof(perf_event_callchain_sample_fixed, sample_id));

  ring_buffer->ReadRawAtOffset(event->ips.data(), offset_of_ips, size_of_ips_in_bytes);

  ring_buffer->ReadRawAtOffset(&event->regs, offset_of_regs_user_struct,
                               sizeof(perf_event_sample_regs_user_all));
  ring_buffer->ReadRawAtOffset(event->stack.data.get(), offset_of_data, dyn_size);
  ring_buffer->SkipRecord(header);
  return event;
}

std::unique_ptr<GenericTracepointPerfEvent> ConsumeGenericTracepointPerfEvent(
    PerfEventRingBuffer* ring_buffer, const perf_event_header& header) {
  auto event = std::make_unique<GenericTracepointPerfEvent>();
  ring_buffer->ReadRawAtOffset(&event->ring_buffer_record, 0, sizeof(perf_event_raw_sample_fixed));
  ring_buffer->SkipRecord(header);
  return event;
}

}  // namespace orbit_linux_tracing
