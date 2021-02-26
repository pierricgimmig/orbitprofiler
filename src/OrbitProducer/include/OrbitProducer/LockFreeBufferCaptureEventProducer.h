// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_
#define ORBIT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_

#include <absl/container/flat_hash_map.h>
#include <google/protobuf/arena.h>

#include <limits>

#include "OrbitBase/Logging.h"
#include "OrbitBase/MakeUniqueForOverwrite.h"
#include "OrbitBase/Profiling.h"
#include "OrbitBase/ThreadUtils.h"
#include "OrbitProducer/CaptureEventProducer.h"
#include "concurrentqueue.h"

class ScopeTimer {
 public:
  ScopeTimer(std::string_view name) {
    name_ = name;
    start_ = orbit_base::CaptureTimestampNs();
  }
  ~ScopeTimer() {
    uint64_t end = orbit_base::CaptureTimestampNs();
    uint64_t duration = end - start_;
    LOG("%s took %.6f ms", name_, static_cast<double>(duration) / 1'000'000);
    messages_to_timers[name_].push_back(duration);
  }
  static void OutputReport() {
    uint64_t now = orbit_base::CaptureTimestampNs();
    if (now - last_report < 1'000'000'000) return;
    LOG("=================");
    LOG("ScopeTimer Report");
    LOG("=================");
    last_report = now;
    for (auto& [name, timers] : messages_to_timers) {
      double avg = 0;
      double min = std::numeric_limits<double>::max();
      double max = 0;
      double num_timers = static_cast<double>(timers.size());
      for (auto timer : timers) {
        double duration_ms = static_cast<double>(timer) / 1'000'000;
        avg += duration_ms / num_timers;
        min = std::min(duration_ms, min);
        max = std::max(duration_ms, max);
      }
      LOG("%s avg:%.6f ms min:%.6f ms max:%.6f ms num_samples:%u", name, avg, min, max,
          timers.size());
    }
  }

 private:
  uint64_t start_;
  std::string name_;
  static inline absl::flat_hash_map<std::string, std::vector<uint64_t>> messages_to_timers;
  static inline uint64_t last_report = 0;
};

namespace orbit_producer {

// This still abstract implementation of CaptureEventProducer provides a lock-free queue where to
// write events with low overhead from the fast path where they are produced.
// Events are enqueued using the methods EnqueueIntermediateEvent(IfCapturing).
//
// Internally, a thread reads from the lock-free queue and sends ProducerCaptureEvents to
// ProducerSideService using the methods provided by the superclass.
//
// The type of the events stored in the lock-free queue is specified by the type parameter
// IntermediateEventT. These events don't need to be ProducerCaptureEvents, nor protobufs at all.
// This is to allow enqueuing objects that are faster to produce than protobufs.
// ProducerCaptureEvents are then built from IntermediateEventT in TranslateIntermediateEvents,
// which subclasses need to implement.
//
// In particular, when hundreds of thousands of events are produced per second, it is recommended
// that IntermediateEventT not be a protobuf or another type that involves heap allocations, as the
// cost of dynamic allocations and de-allocations can add up quickly.
template <typename IntermediateEventT>
class LockFreeBufferCaptureEventProducer : public CaptureEventProducer {
 public:
  void BuildAndStart(const std::shared_ptr<grpc::Channel>& channel) override {
    CaptureEventProducer::BuildAndStart(channel);

    forwarder_thread_ = std::thread{&LockFreeBufferCaptureEventProducer::ForwarderThread, this};
  }

  void ShutdownAndWait() override {
    shutdown_requested_ = true;

    CHECK(forwarder_thread_.joinable());
    forwarder_thread_.join();

    CaptureEventProducer::ShutdownAndWait();
  }

  void EnqueueIntermediateEvent(const IntermediateEventT& event) {
    lock_free_queue_.enqueue(event);
  }

  void EnqueueIntermediateEvent(IntermediateEventT&& event) {
    lock_free_queue_.enqueue(std::move(event));
  }

  bool EnqueueIntermediateEventIfCapturing(
      const std::function<IntermediateEventT()>& event_builder_if_capturing) {
    if (IsCapturing()) {
      lock_free_queue_.enqueue(event_builder_if_capturing());
      return true;
    }
    return false;
  }

 protected:
  void OnCaptureStart(orbit_grpc_protos::CaptureOptions /*capture_options*/) override {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerStatus::kShouldSendEvents;
  }

  void OnCaptureStop() override {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerStatus::kShouldNotifyAllEventsSent;
  }

  void OnCaptureFinished() override {
    absl::MutexLock lock{&status_mutex_};
    status_ = ProducerStatus::kShouldDropEvents;
  }

  // Subclasses need to implement this method to convert an `IntermediateEventT` enqueued in the
  // internal lock-free buffer to a `CaptureEvent` to be sent to ProducerSideService.
  // The `CaptureEvent` must be created in the Arena using `google::protobuf::Arena::CreateMessage`
  // from <google/protobuf/arena.h>. The pointer provided by `CreateMessage` should be returned.
  // This optimizes memory allocations and cache efficiency. But keep in mind that:
  // - `string` and `bytes` fields (both of which use `std::string` internally) still get heap
  //   allocated no matter what;
  // - If `IntermediateEventT` is itself a `ProducerCaptureEvent`, or the type of one of its fields,
  //   attempting to move from it into the Arena-allocated `ProducerCaptureEvent` will silently
  //   result in a deep copy.
  [[nodiscard]] virtual std::vector<orbit_grpc_protos::ProducerCaptureEvent*>
  TranslateIntermediateEvents(IntermediateEventT* moveable_intermediate_events, size_t num_events,
                              google::protobuf::Arena* arena) = 0;

  [[nodiscard]] virtual orbit_grpc_protos::ProducerCaptureEvent* TranslateSingleIntermediateEvent(
      IntermediateEventT&& intermediate_event, google::protobuf::Arena* arena) {
    (void)intermediate_event;
    (void)arena;
    return nullptr;
  }

 private:
  void ForwarderThread() {
    orbit_base::SetCurrentThreadName("ForwarderThread");

    constexpr uint64_t kMaxEventsPerRequest = 10'000;
    std::vector<IntermediateEventT> dequeued_events(kMaxEventsPerRequest);

    // Pre-allocate and always reuse the same 1 MB chunk of memory as the first block of each Arena
    // instance in the loop below. This is a small but measurable performance improvement.
    google::protobuf::ArenaOptions arena_options;
    constexpr size_t kArenaInitialBlockSize = 1024 * 1024;
    auto arena_initial_block = make_unique_for_overwrite<char[]>(kArenaInitialBlockSize);
    arena_options.initial_block = arena_initial_block.get();
    arena_options.initial_block_size = kArenaInitialBlockSize;

    while (!shutdown_requested_) {
      while (true) {
        size_t dequeued_event_count =
            lock_free_queue_.try_dequeue_bulk(dequeued_events.begin(), kMaxEventsPerRequest);
        bool queue_was_emptied = dequeued_event_count < kMaxEventsPerRequest;

        ProducerStatus current_status;
        {
          absl::MutexLock lock{&status_mutex_};
          current_status = status_;
          if (status_ == ProducerStatus::kShouldNotifyAllEventsSent && queue_was_emptied) {
            // We are about to send AllEventsSent: update status_ while we hold the mutex.
            status_ = ProducerStatus::kShouldDropEvents;
          }
        }

        if ((current_status == ProducerStatus::kShouldSendEvents ||
             current_status == ProducerStatus::kShouldNotifyAllEventsSent) &&
            dequeued_event_count > 0) {
          google::protobuf::Arena arena{arena_options};
          auto* send_request = google::protobuf::Arena::CreateMessage<
              orbit_grpc_protos::ReceiveCommandsAndSendEventsRequest>(&arena);

          std::vector<orbit_grpc_protos::ProducerCaptureEvent*> translated_events;
          translated_events.resize(dequeued_event_count);
          LOG("/====");

          {
            ScopeTimer t("TranslateIntermediateEvents one by one");
            for (size_t i = 0; i < dequeued_event_count; ++i) {
              auto single_event =
                  TranslateSingleIntermediateEvent(std::move(dequeued_events[i]), &arena);
              translated_events[i] = single_event;
            }
          }

          translated_events.clear();
          translated_events.reserve(dequeued_event_count);

          {
            ScopeTimer t("TranslateIntermediateEvents in bulk");
            translated_events =
                TranslateIntermediateEvents(dequeued_events.data(), dequeued_event_count, &arena);
          }

          LOG("num_events: %u", dequeued_event_count);
          LOG("\\====");

          ScopeTimer::OutputReport();

          auto* capture_events =
              send_request->mutable_buffered_capture_events()->mutable_capture_events();
          capture_events->Reserve(translated_events.size());
          for (orbit_grpc_protos::ProducerCaptureEvent* translated_event : translated_events) {
            capture_events->AddAllocated(translated_event);
          }

          if (!SendCaptureEvents(*send_request)) {
            ERROR("Forwarding %lu CaptureEvents", dequeued_event_count);
            break;
          }
        }

        if (current_status == ProducerStatus::kShouldNotifyAllEventsSent && queue_was_emptied) {
          // lock_free_queue_ is now empty and status_ == kShouldNotifyAllEventsSent,
          // send AllEventsSent. status_ has already been changed to kShouldDropEvents.
          if (!NotifyAllEventsSent()) {
            ERROR("Notifying that all CaptureEvents have been sent");
          }
          break;
        }

        // Note that if current_status == ProducerStatus::kShouldDropEvents
        // the events extracted from the lock_free_queue_ will just be dropped.

        if (queue_was_emptied) {
          break;
        }
      }

      static constexpr std::chrono::duration kSleepOnEmptyQueue = std::chrono::milliseconds{10};
      // Wait for lock_free_queue_ to fill up with new CaptureEvents.
      std::this_thread::sleep_for(kSleepOnEmptyQueue);
    }
  }

 private:
  moodycamel::ConcurrentQueue<IntermediateEventT> lock_free_queue_;

  std::thread forwarder_thread_;
  std::atomic<bool> shutdown_requested_ = false;

  enum class ProducerStatus { kShouldSendEvents, kShouldNotifyAllEventsSent, kShouldDropEvents };
  ProducerStatus status_ = ProducerStatus::kShouldDropEvents;
  absl::Mutex status_mutex_;
};

}  // namespace orbit_producer

#endif  // ORBIT_PRODUCER_LOCK_FREE_BUFFER_CAPTURE_EVENT_PRODUCER_H_
