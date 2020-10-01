// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_CAPTURE_CLIENT_CAPTURE_CLIENT_H_
#define ORBIT_CAPTURE_CLIENT_CAPTURE_CLIENT_H_

#include "CaptureListener.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/Result.h"
#include "OrbitBase/ThreadPool.h"
#include "OrbitClientData/ProcessData.h"
#include "TracepointCustom.h"
#include "absl/container/flat_hash_set.h"
#include "capture_data.pb.h"
#include "grpcpp/channel.h"
#include "services.grpc.pb.h"

class CaptureClient {
 public:
  enum class State { kStopped = 0, kStarting, kStarted, kStopping };

  explicit CaptureClient(const std::shared_ptr<grpc::Channel>& channel,
                         CaptureListener* capture_listener)
      : capture_service_{orbit_grpc_protos::CaptureService::NewStub(channel)},
        capture_listener_{capture_listener},
        state_{State::kStopped},
        force_stop_{false} {
    CHECK(capture_listener_ != nullptr);
  }

  [[nodiscard]] ErrorMessageOr<void> StartCapture(
      ThreadPool* thread_pool, const ProcessData& process,
      absl::flat_hash_map<std::string, ModuleData*> module_map,
      absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> selected_functions,
      TracepointInfoSet selected_tracepoints);

  // Returns true if stop was initiated and false otherwise.
  // The latter can happen if for example the stop was already
  // initiated.
  //
  // This call may block if the capture is in kStarting state,
  // it will wait until capture is started or failed to start.
  [[nodiscard]] bool StopCapture();

  [[nodiscard]] State state() const {
    absl::MutexLock lock(&state_mutex_);
    return state_;
  }

  [[nodiscard]] bool IsCapturing() const {
    absl::MutexLock lock(&state_mutex_);
    return state_ != State::kStopped;
  }

 private:
  void Capture(ProcessData&& process, absl::flat_hash_map<std::string, ModuleData*> module_map,
               absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> selected_functions,
               TracepointInfoSet selected_tracepoints);

  void FinishCapture();

  std::unique_ptr<orbit_grpc_protos::CaptureService::Stub> capture_service_;
  std::unique_ptr<grpc::ClientReaderWriter<orbit_grpc_protos::CaptureRequest,
                                           orbit_grpc_protos::CaptureResponse>>
      reader_writer_;

  CaptureListener* capture_listener_ = nullptr;

  mutable absl::Mutex state_mutex_;
  State state_;
  std::atomic<bool> force_stop_;
};

#endif  // ORBIT_GL_CAPTURE_CLIENT_H_
