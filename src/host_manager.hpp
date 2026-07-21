#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "channel.hpp"
#include "host.hpp"
#include "job.hpp"

// Process-level manager: owns Hosts, accepts submit/cancel from any thread.
// See docs/host-job-scheduler.md.
//
// Each Host runs a single thread. Jobs are dispatched concurrently on that
// thread (cooperative): the dispatcher spawns each job and immediately
// receives the next; multiple jobs may be awaiting IO/Promises at once.

class HostManager {
 public:
  struct CreateOptions {
    // Empty -> UUID host id.
    std::string id;
    // Max jobs simultaneously executing (awaiting) on one Host. Dispatcher
    // waits when full. Default allows broad IO concurrency.
    int max_in_flight = 64;
    std::function<void(Host&)> setup;
  };

  HostManager() = default;
  ~HostManager();

  HostManager(const HostManager&) = delete;
  HostManager& operator=(const HostManager&) = delete;

  // Default-constructed CreateOptions (avoid default arg on nested type:
  // clang default_member_initializer_not_yet_parsed).
  std::string create_host();
  std::string create_host(CreateOptions opts);

  // Convenience overloads.
  std::string create_host(std::function<void(Host&)> setup);
  std::string create_host(std::string id, std::function<void(Host&)> setup = {});

  bool destroy_host(const std::string& host_id);

  struct SubmitHandle {
    uint64_t task_id = 0;
    co::oneshot::Receiver<job::Result> rx;
  };

  std::optional<SubmitHandle> submit(
    const std::string& host_id,
    std::string fn_name,
    job::Args args
    );

  bool cancel(uint64_t task_id);

  bool has_host(const std::string& host_id) const;

 private:
  // Per-Host run state shared by dispatcher and in-flight jobs (Host thread).
  struct HostRunState {
    std::atomic<int> in_flight{0};
    int max_in_flight = 64;
  };

  struct Slot {
    std::unique_ptr<Host> host;
    co::mpsc::Sender<job::Job> job_tx;
    std::shared_ptr<HostRunState> run;
    std::thread thread;
  };

  static void run_host_thread(Host* host);
  void start_job_worker(
    Host* host,
    std::shared_ptr<HostRunState> run,
    co::mpsc::Receiver<job::Job> rx
    );
  static async_simple::coro::Lazy<void> execute_job(Host* host, job::Job j);
  static async_simple::coro::Lazy<void> run_one_job(
    HostManager* mgr,
    Host* host,
    std::shared_ptr<HostRunState> run,
    uint64_t task_id,
    job::Job j
    );
  void forget_task(uint64_t task_id);
  static async_simple::coro::Lazy<void> job_worker_loop(
    HostManager* mgr,
    Host* host,
    std::shared_ptr<HostRunState> run,
    co::mpsc::Receiver<job::Job> rx
    );

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<Slot>> hosts_;
  std::unordered_map<uint64_t, std::shared_ptr<std::atomic_bool>> cancels_;
  uint64_t next_task_id_ = 1;
};
