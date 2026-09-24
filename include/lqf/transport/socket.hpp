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

#ifndef LQF_TRANSPORT_SOCKET_HPP
#define LQF_TRANSPORT_SOCKET_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/export.hpp"

#if defined(_WIN32)
using lqf_native_socket = std::uintptr_t;
inline constexpr lqf_native_socket lqf_invalid_socket = static_cast<lqf_native_socket>(~0ULL);
#else
using lqf_native_socket = int;
inline constexpr lqf_native_socket lqf_invalid_socket = -1;
#endif

namespace lqf {

// Real loopback TCP. Threads are not processes and a queue is not a transport:
// the multiprocess proofs use this socket layer across two independent OS
// processes.
class LQF_API SocketRuntime {
 public:
  static Status ensure();
  static void release() noexcept;
};

class LQF_API Socket {
 public:
  Socket() = default;
  explicit Socket(lqf_native_socket handle) : handle_(handle) {}
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ != lqf_invalid_socket; }
  [[nodiscard]] lqf_native_socket native() const noexcept { return handle_; }

  static Outcome<Socket> listen_loopback(u16 port, int backlog, u16& bound_port);
  static Outcome<Socket> connect_loopback(u16 port);
  [[nodiscard]] Outcome<Socket> accept() const;

  Status send_all(std::string_view data) const;
  // Reads exactly size bytes or fails. A peer that closes mid-message is a
  // protocol error, never a partial success.
  Status recv_exact(void* out, std::size_t size) const;
  [[nodiscard]] Outcome<std::size_t> recv_some(void* out, std::size_t size) const;

  void shutdown_both() noexcept;
  void close() noexcept;

  [[nodiscard]] u16 local_port() const;
  [[nodiscard]] std::string peer_text() const;

  // Waits until the socket is readable or the wakeup channel fires. There is no
  // timeout: cancellation is a real signal, not a deadline.
  static Status wait_readable(lqf_native_socket socket, lqf_native_socket wake, bool& socket_ready,
                              bool& wake_ready);

 private:
  lqf_native_socket handle_{lqf_invalid_socket};
};

// Portable self-pipe built from a connected loopback TCP pair. It exists so a
// blocking wait can be interrupted by a real shutdown signal on every platform.
class LQF_API WakeupPair {
 public:
  WakeupPair() = default;
  ~WakeupPair();
  WakeupPair(const WakeupPair&) = delete;
  WakeupPair& operator=(const WakeupPair&) = delete;
  WakeupPair(WakeupPair&&) noexcept = default;
  WakeupPair& operator=(WakeupPair&&) noexcept = default;

  static Outcome<WakeupPair> create();
  Status wake();
  Status drain();
  void close();
  [[nodiscard]] lqf_native_socket read_socket() const noexcept { return reader_.native(); }

 private:
  Socket reader_{};
  Socket writer_{};
};

LQF_API Status last_socket_error_status(const char* what);

}  // namespace lqf

#endif  // LQF_TRANSPORT_SOCKET_HPP
