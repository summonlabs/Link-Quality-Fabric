// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LQF_TRANSPORT_SERVER_HPP
#define LQF_TRANSPORT_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lqf/core/lock_tracker.hpp"
#include "lqf/runtime/fabric.hpp"
#include "lqf/transport/frame.hpp"
#include "lqf/transport/socket.hpp"

namespace lqf {

struct ServerConfig {
  std::string bind_address{"127.0.0.1"};
  u16 port{0};
  FrameLimits frames{};
  CodecLimits codec{};
  std::size_t max_connections{64};
  // An empty token refuses every shutdown request: the transport never lets a
  // peer stop the runtime unless the operator armed it.
  std::string shutdown_token{};
};

struct ServerStats {
  u64 connections_accepted{0};
  u64 connections_rejected{0};
  u64 frames_in{0};
  u64 frames_out{0};
  u64 protocol_errors{0};
  u64 requests_refused{0};
  u64 bytes_in{0};
  u64 bytes_out{0};
  std::size_t active_connections{0};
  bool running{false};
  bool stop_requested{false};
  u16 bound_port{0};
};

// A real loopback TCP server over a framed transport. Shutdown is a signal, not
// a deadline: the accept loop waits on a wakeup channel, every connection socket
// is shut down explicitly, and every thread is joined.
class LQF_API FabricServer {
 public:
  FabricServer(std::shared_ptr<Fabric> fabric, ServerConfig config);
  ~FabricServer();
  FabricServer(const FabricServer&) = delete;
  FabricServer& operator=(const FabricServer&) = delete;

  Status start();
  Status stop();
  void wait();
  Status request_stop();

  [[nodiscard]] u16 port() const noexcept { return port_; }
  [[nodiscard]] ServerStats stats() const;
  [[nodiscard]] bool running() const noexcept { return running_.load(); }

 private:
  struct Connection {
    std::thread thread{};
    std::shared_ptr<std::atomic<bool>> done{};
    // Per connection cancellation channel. A blocking recv on Windows is not
    // reliably woken by shutdown from another thread, so every read waits for
    // readability first and this channel is the real interrupt.
    std::shared_ptr<WakeupPair> wake{};
    lqf_native_socket handle{lqf_invalid_socket};
  };

  void accept_loop();
  void connection_loop(Socket socket, std::shared_ptr<WakeupPair> wake,
                       std::shared_ptr<std::atomic<bool>> done);
  // Reads exactly size bytes, waiting for readability before each chunk so a
  // shutdown signal interrupts even a peer that stalls mid message.
  Status read_exact_interruptible(const Socket& socket, WakeupPair& wake, void* out,
                                  std::size_t size);
  Outcome<Frame> dispatch(const Frame& request, bool& stop_after);
  Status send_frame(const Socket& socket, MessageType type, std::string body);
  Status send_status(const Socket& socket, const Status& status);
  void reap_finished_locked();

  // Statistics helpers. Each one takes and releases the statistics lock, so no
  // caller can ever hold it while calling another helper: the lock is never
  // re-entered.
  void note_connection_accepted();
  void note_connection_rejected();
  void note_frames_in(std::size_t bytes);
  void note_frames_out(std::size_t bytes);
  void note_protocol_error();
  void note_request_refused();

  std::shared_ptr<Fabric> fabric_{};
  const ServerConfig config_{};
  Socket listener_{};
  WakeupPair wake_{};
  std::thread accept_thread_{};
  mutable locks::TrackedMutex connections_mutex_{"server_connections"};
  std::vector<Connection> connections_{};
  std::mutex wait_mutex_{};
  std::condition_variable wait_cv_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::size_t> active_connections_{0};
  u16 port_{0};
  // Audited: a re-entrant acquisition of this lock is a defect and the runtime
  // lock tracker reports it instead of deadlocking.
  mutable locks::TrackedMutex stats_mutex_{"server_stats"};
  ServerStats stats_{};
};

}  // namespace lqf

#endif  // LQF_TRANSPORT_SERVER_HPP
