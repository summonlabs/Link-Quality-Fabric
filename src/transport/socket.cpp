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

#include "lqf/transport/socket.hpp"

#include <atomic>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lqf {
namespace {

std::atomic<int> g_winsock_references{0};

#if defined(_WIN32)
SOCKET to_native(lqf_native_socket handle) { return static_cast<SOCKET>(handle); }
lqf_native_socket from_native(SOCKET handle) { return static_cast<lqf_native_socket>(handle); }
#else
int to_native(lqf_native_socket handle) { return static_cast<int>(handle); }
lqf_native_socket from_native(int handle) { return static_cast<lqf_native_socket>(handle); }
#endif

int last_error() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool would_block(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EWOULDBLOCK || error == EAGAIN || error == EINTR;
#endif
}

}  // namespace

Status last_socket_error_status(const char* what) {
  return Status::error(StatusCode::Io,
                       std::string(what) + " failed with socket error " +
                           std::to_string(last_error()));
}

Status SocketRuntime::ensure() {
#if defined(_WIN32)
  if (g_winsock_references.fetch_add(1) == 0) {
    WSADATA data;
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      g_winsock_references.fetch_sub(1);
      return Status::error(StatusCode::Io,
                           "WSAStartup failed with error " + std::to_string(result));
    }
  }
#else
  g_winsock_references.fetch_add(1);
#endif
  return Status::success();
}

void SocketRuntime::release() noexcept {
#if defined(_WIN32)
  if (g_winsock_references.fetch_sub(1) == 1) {
    WSACleanup();
  }
#else
  g_winsock_references.fetch_sub(1);
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = lqf_invalid_socket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = lqf_invalid_socket;
  }
  return *this;
}

Outcome<Socket> Socket::listen_loopback(u16 port, int backlog, u16& bound_port) {
  const Status runtime = SocketRuntime::ensure();
  if (!runtime.ok()) {
    return runtime;
  }
#if defined(_WIN32)
  SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return last_socket_error_status("socket");
  }
#else
  const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle < 0) {
    return last_socket_error_status("socket");
  }
#endif
  Socket socket(from_native(handle));
  int reuse = 1;
  (void)::setsockopt(to_native(socket.handle_), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(to_native(socket.handle_), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return last_socket_error_status("bind");
  }
  if (::listen(to_native(socket.handle_), backlog) != 0) {
    return last_socket_error_status("listen");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(to_native(socket.handle_), reinterpret_cast<sockaddr*>(&bound), &bound_length) !=
      0) {
    return last_socket_error_status("getsockname");
  }
  bound_port = ntohs(bound.sin_port);
  return socket;
}

Outcome<Socket> Socket::connect_loopback(u16 port) {
  const Status runtime = SocketRuntime::ensure();
  if (!runtime.ok()) {
    return runtime;
  }
#if defined(_WIN32)
  SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == INVALID_SOCKET) {
    return last_socket_error_status("socket");
  }
#else
  const int handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle < 0) {
    return last_socket_error_status("socket");
  }
#endif
  Socket socket(from_native(handle));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(to_native(socket.handle_), reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    return last_socket_error_status("connect");
  }
  return socket;
}

Outcome<Socket> Socket::accept() const {
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const auto handle = ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
#if defined(_WIN32)
  if (handle == INVALID_SOCKET) {
    return last_socket_error_status("accept");
  }
#else
  if (handle < 0) {
    return last_socket_error_status("accept");
  }
#endif
  return Socket(from_native(handle));
}

Status Socket::send_all(std::string_view data) const {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = remaining > 1U << 20U ? static_cast<int>(1U << 20U)
                                            : static_cast<int>(remaining);
    const auto result = ::send(to_native(handle_), data.data() + sent, chunk, 0);
    if (result <= 0) {
      return last_socket_error_status("send");
    }
    sent += static_cast<std::size_t>(result);
  }
  return Status::success();
}

Status Socket::recv_exact(void* out, std::size_t size) const {
  auto* bytes = static_cast<unsigned char*>(out);
  std::size_t received = 0;
  while (received < size) {
    const std::size_t remaining = size - received;
    const int chunk = remaining > 1U << 20U ? static_cast<int>(1U << 20U)
                                            : static_cast<int>(remaining);
    const auto result = ::recv(to_native(handle_), reinterpret_cast<char*>(bytes + received), chunk, 0);
    if (result == 0) {
      return Status::error(StatusCode::Cancelled, "peer closed the connection mid message");
    }
    if (result < 0) {
      const int error = last_error();
      if (would_block(error)) {
        continue;
      }
      return last_socket_error_status("recv");
    }
    received += static_cast<std::size_t>(result);
  }
  return Status::success();
}

Outcome<std::size_t> Socket::recv_some(void* out, std::size_t size) const {
  const int chunk = size > 1U << 20U ? static_cast<int>(1U << 20U) : static_cast<int>(size);
  const auto result = ::recv(to_native(handle_), static_cast<char*>(out), chunk, 0);
  if (result == 0) {
    return Status::error(StatusCode::Cancelled, "peer closed the connection");
  }
  if (result < 0) {
    return last_socket_error_status("recv");
  }
  return static_cast<std::size_t>(result);
}

void Socket::shutdown_both() noexcept {
  if (handle_ == lqf_invalid_socket) {
    return;
  }
#if defined(_WIN32)
  (void)::shutdown(to_native(handle_), SD_BOTH);
#else
  (void)::shutdown(to_native(handle_), SHUT_RDWR);
#endif
}

void Socket::close() noexcept {
  if (handle_ == lqf_invalid_socket) {
    return;
  }
#if defined(_WIN32)
  (void)::closesocket(to_native(handle_));
#else
  (void)::close(to_native(handle_));
#endif
  handle_ = lqf_invalid_socket;
}

u16 Socket::local_port() const {
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(to_native(handle_), reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    return 0;
  }
  return ntohs(bound.sin_port);
}

std::string Socket::peer_text() const {
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0) {
    return "unknown";
  }
  char buffer[INET_ADDRSTRLEN] = {0};
  if (::inet_ntop(AF_INET, &peer.sin_addr, buffer, sizeof(buffer)) == nullptr) {
    return "unknown";
  }
  return std::string(buffer) + ":" + std::to_string(ntohs(peer.sin_port));
}

Status Socket::wait_readable(lqf_native_socket socket, lqf_native_socket wake, bool& socket_ready,
                             bool& wake_ready) {
  socket_ready = false;
  wake_ready = false;
  for (;;) {
    fd_set read_set;
    FD_ZERO(&read_set);
    lqf_native_socket highest = 0;
    if (socket != lqf_invalid_socket) {
      FD_SET(to_native(socket), &read_set);
      highest = socket;
    }
    if (wake != lqf_invalid_socket) {
      FD_SET(to_native(wake), &read_set);
      if (wake > highest) {
        highest = wake;
      }
    }
    if (socket == lqf_invalid_socket && wake == lqf_invalid_socket) {
      return Status::error(StatusCode::Internal, "no socket to wait on");
    }
#if defined(_WIN32)
    const int result = ::select(0, &read_set, nullptr, nullptr, nullptr);
#else
    const int result = ::select(static_cast<int>(highest) + 1, &read_set, nullptr, nullptr, nullptr);
#endif
    if (result < 0) {
      const int error = last_error();
      if (would_block(error)) {
        continue;
      }
      return last_socket_error_status("select");
    }
    if (socket != lqf_invalid_socket && FD_ISSET(to_native(socket), &read_set) != 0) {
      socket_ready = true;
    }
    if (wake != lqf_invalid_socket && FD_ISSET(to_native(wake), &read_set) != 0) {
      wake_ready = true;
    }
    if (socket_ready || wake_ready) {
      return Status::success();
    }
  }
}

WakeupPair::~WakeupPair() { close(); }

Outcome<WakeupPair> WakeupPair::create() {
  u16 port = 0;
  Outcome<Socket> listener = Socket::listen_loopback(0, 1, port);
  if (!listener.ok()) {
    return listener.status();
  }
  Outcome<Socket> writer = Socket::connect_loopback(port);
  if (!writer.ok()) {
    return writer.status();
  }
  Outcome<Socket> reader = listener.value().accept();
  if (!reader.ok()) {
    return reader.status();
  }
  WakeupPair pair;
  pair.reader_ = std::move(reader.value());
  pair.writer_ = std::move(writer.value());
  return pair;
}

Status WakeupPair::wake() {
  if (!writer_.valid()) {
    return Status::error(StatusCode::Unavailable, "wakeup channel is closed");
  }
  const char byte = 1;
  return writer_.send_all(std::string_view(&byte, 1));
}

Status WakeupPair::drain() {
  if (!reader_.valid()) {
    return Status::error(StatusCode::Unavailable, "wakeup channel is closed");
  }
  char buffer[64];
  const Outcome<std::size_t> read = reader_.recv_some(buffer, sizeof(buffer));
  if (!read.ok()) {
    return read.status();
  }
  return Status::success();
}

void WakeupPair::close() {
  reader_.close();
  writer_.close();
}

}  // namespace lqf
