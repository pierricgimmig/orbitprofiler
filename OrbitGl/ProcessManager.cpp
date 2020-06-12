// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ProcessManager.h"

#include <chrono>
#include <memory>
#include <string>

#include "OrbitBase/Logging.h"
#include "grpcpp/grpcpp.h"
#include "services.grpc.pb.h"

namespace {

constexpr uint64_t kGrpcCallTimeoutMilliseconds = 1000;

class ProcessManagerImpl final : public ProcessManager {
 public:
  explicit ProcessManagerImpl(std::shared_ptr<grpc::Channel> channel,
                              absl::Duration refresh_timeout);

  void SetProcessListUpdateListener(
      const std::function<void(ProcessManager*)>& listener) override;
  std::vector<ProcessInfo> process_list() const override;
  outcome::result<std::vector<ModuleInfo>, std::string> GetModuleList(
      uint32_t pid) override;
  void Start();
  void Shutdown() override;

 private:
  std::unique_ptr<grpc::ClientContext> CreateContext();
  void WorkerFunction();

  std::unique_ptr<ProcessService::Stub> process_service_;

  absl::Duration refresh_timeout_;
  absl::Mutex shutdown_mutex_;
  bool shutdown_initiated_;

  mutable absl::Mutex mutex_;
  std::vector<ProcessInfo> process_list_;
  std::function<void(ProcessManager*)> process_list_update_listener_;

  std::thread worker_thread_;
};

ProcessManagerImpl::ProcessManagerImpl(std::shared_ptr<grpc::Channel> channel,
                                       absl::Duration refresh_timeout)
    : process_service_(ProcessService::NewStub(channel)),
      refresh_timeout_(refresh_timeout),
      shutdown_initiated_(false) {}

void ProcessManagerImpl::SetProcessListUpdateListener(
    const std::function<void(ProcessManager*)>& listener) {
  absl::MutexLock lock(&mutex_);
  process_list_update_listener_ = listener;
}

outcome::result<std::vector<ModuleInfo>, std::string>
ProcessManagerImpl::GetModuleList(uint32_t pid) {
  GetModuleListRequest request;
  GetModuleListResponse response;
  request.set_process_id(pid);

  std::unique_ptr<grpc::ClientContext> context = CreateContext();
  grpc::Status status =
      process_service_->GetModuleList(context.get(), request, &response);

  if (!status.ok()) {
    return status.error_message();
  }

  const auto& modules = response.modules();

  return std::vector<ModuleInfo>(modules.begin(), modules.end());
}

std::vector<ProcessInfo> ProcessManagerImpl::process_list() const {
  absl::MutexLock lock(&mutex_);
  return process_list_;
}

void ProcessManagerImpl::Start() {
  CHECK(!worker_thread_.joinable());
  worker_thread_ = std::thread([this] { WorkerFunction(); });
}

void ProcessManagerImpl::Shutdown() {
  shutdown_mutex_.Lock();
  shutdown_initiated_ = true;
  shutdown_mutex_.Unlock();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

std::unique_ptr<grpc::ClientContext> ProcessManagerImpl::CreateContext() {
  std::unique_ptr<grpc::ClientContext> context =
      std::make_unique<grpc::ClientContext>();

  std::chrono::time_point deadline =
      std::chrono::system_clock::now() +
      std::chrono::milliseconds(kGrpcCallTimeoutMilliseconds);
  context->set_deadline(deadline);

  return context;
}

bool IsTrue(bool* var) { return *var; }

void ProcessManagerImpl::WorkerFunction() {
  while (true) {
    if (shutdown_mutex_.LockWhenWithTimeout(
            absl::Condition(IsTrue, &shutdown_initiated_), refresh_timeout_)) {
      // Shutdown was initiated we need to exit
      shutdown_mutex_.Unlock();
      return;
    }
    shutdown_mutex_.Unlock();
    // Timeout expired - refresh the list

    GetProcessListRequest request;
    GetProcessListResponse response;
    std::unique_ptr<grpc::ClientContext> context = CreateContext();

    grpc::Status status =
        process_service_->GetProcessList(context.get(), request, &response);
    if (!status.ok()) {
      ERROR("Grpc call failed: %s", status.error_message());
      continue;
    }

    absl::MutexLock callback_lock(&mutex_);
    const auto& processes = response.processes();
    process_list_.assign(processes.begin(), processes.end());
    if (process_list_update_listener_) {
      process_list_update_listener_(this);
    }
  }
}

}  // namespace

std::unique_ptr<ProcessManager> ProcessManager::Create(
    std::shared_ptr<grpc::Channel> channel, absl::Duration refresh_timeout) {
  std::unique_ptr<ProcessManagerImpl> impl =
      std::make_unique<ProcessManagerImpl>(channel, refresh_timeout);
  impl->Start();
  return impl;
}
