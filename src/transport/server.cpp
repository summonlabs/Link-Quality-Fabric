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

#include "lqf/transport/server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "lqf/core/text.hpp"
#include "lqf/persist/codec.hpp"
#include "lqf/version.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace lqf {
namespace {

template <class T>
Status decode_request(const std::string& body, T& value, const CodecLimits& limits) {
  Reader reader(body, limits);
  Decoder decoder(reader, limits);
  decoder.field(value);
  if (!decoder.ok()) {
    return reader.status();
  }
  if (!reader.empty()) {
    return Status::error(StatusCode::Protocol, "request has trailing bytes");
  }
  return Status::success();
}

template <class T>
Status encode_reply(const T& value, std::string& body, const CodecLimits& limits) {
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(value);
  if (!encoder.ok()) {
    return writer.status();
  }
  body = writer.release();
  return Status::success();
}

}  // namespace

FabricServer::FabricServer(std::shared_ptr<Fabric> fabric, ServerConfig config)
    : fabric_(std::move(fabric)), config_(std::move(config)) {}

FabricServer::~FabricServer() {
  const Status status = stop();
  (void)status;
}

Status FabricServer::start() {
  if (fabric_ == nullptr) {
    return Status::error(StatusCode::Invalid, "a fabric instance is required");
  }
  if (running_.load()) {
    return Status::error(StatusCode::Busy, "the server is already running");
  }
  if (config_.max_connections == 0) {
    return Status::error(StatusCode::Invalid, "max_connections must be at least one");
  }
  Outcome<WakeupPair> wake = WakeupPair::create();
  if (!wake.ok()) {
    return wake.status();
  }
  wake_ = std::move(wake.value());

  Outcome<Socket> listener = Socket::listen_loopback(config_.port, 64, port_);
  if (!listener.ok()) {
    return listener.status();
  }
  listener_ = std::move(listener.value());
  {
    locks::UniqueLock guard(stats_mutex_);
    stats_.bound_port = port_;
    stats_.running = true;
    stats_.stop_requested = false;
  }
  stop_requested_.store(false);
  running_.store(true);
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return Status::success();
}

void FabricServer::note_connection_accepted() {
  locks::UniqueLock guard(stats_mutex_);
  stats_.connections_accepted += 1;
}

void FabricServer::note_connection_rejected() {
  locks::UniqueLock guard(stats_mutex_);
  stats_.connections_rejected += 1;
}

void FabricServer::note_frames_in(std::size_t bytes) {
  locks::UniqueLock guard(stats_mutex_);
  stats_.frames_in += 1;
  stats_.bytes_in += bytes;
}

void FabricServer::note_frames_out(std::size_t bytes) {
  locks::UniqueLock guard(stats_mutex_);
  stats_.frames_out += 1;
  stats_.bytes_out += bytes;
}

void FabricServer::note_protocol_error() {
  locks::UniqueLock guard(stats_mutex_);
  stats_.protocol_errors += 1;
}

void FabricServer::note_request_refused() {
  locks::UniqueLock guard(stats_mutex_);
  stats_.requests_refused += 1;
}

void FabricServer::accept_loop() {
  for (;;) {
    if (stop_requested_.load()) {
      break;
    }
    {
      locks::UniqueLock guard(connections_mutex_);
      reap_finished_locked();
    }
    bool listener_ready = false;
    bool wake_ready = false;
    const Status waited =
        Socket::wait_readable(listener_.native(), wake_.read_socket(), listener_ready, wake_ready);
    if (!waited.ok()) {
      break;
    }
    if (wake_ready) {
      const Status drained = wake_.drain();
      (void)drained;
      continue;
    }
    if (!listener_ready) {
      continue;
    }
    if (active_connections_.load() >= config_.max_connections) {
      Outcome<Socket> rejected = listener_.accept();
      note_connection_rejected();
      if (rejected.ok()) {
        const Status sent = send_status(rejected.value(),
                                        Status::error(StatusCode::Busy,
                                                      "connection capacity reached; the request "
                                                      "was refused"));
        (void)sent;
      }
      continue;
    }
    Outcome<Socket> accepted = listener_.accept();
    if (!accepted.ok()) {
      continue;
    }
    Outcome<WakeupPair> channel = WakeupPair::create();
    if (!channel.ok()) {
      const Status sent = send_status(accepted.value(), channel.status());
      (void)sent;
      continue;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto wake = std::make_shared<WakeupPair>(std::move(channel.value()));
    Socket socket = std::move(accepted.value());
    const lqf_native_socket handle = socket.native();
    locks::UniqueLock guard(connections_mutex_);
    connections_.push_back(Connection{});
    Connection& connection = connections_.back();
    connection.done = done;
    connection.wake = wake;
    connection.handle = handle;
    active_connections_.fetch_add(1);
    note_connection_accepted();
    connection.thread = std::thread([this, socket = std::move(socket), wake, done]() mutable {
      connection_loop(std::move(socket), std::move(wake), done);
    });
  }
  listener_.close();
  {
    locks::UniqueLock guard(stats_mutex_);
    stats_.running = false;
  }
  running_.store(false);
}

void FabricServer::reap_finished_locked() {
  for (auto entry = connections_.begin(); entry != connections_.end();) {
    if (entry->done->load()) {
      if (entry->thread.joinable()) {
        entry->thread.join();
      }
      entry = connections_.erase(entry);
    } else {
      ++entry;
    }
  }
}

Status FabricServer::send_frame(const Socket& socket, MessageType type, std::string body) {
  Frame frame;
  frame.type = type;
  frame.body = std::move(body);
  std::string encoded;
  const Status status = encode_frame(frame, encoded, config_.frames);
  if (!status.ok()) {
    return status;
  }
  const Status sent = socket.send_all(encoded);
  if (sent.ok()) {
    note_frames_out(encoded.size());
  }
  return sent;
}

Status FabricServer::send_status(const Socket& socket, const Status& status) {
  ErrorReply reply;
  reply.code = status.code();
  reply.message = status.message();
  std::string body;
  const Status encoded = encode_error_reply(reply, body);
  if (!encoded.ok()) {
    return encoded;
  }
  note_request_refused();
  return send_frame(socket, MessageType::Error, std::move(body));
}

Outcome<Frame> FabricServer::dispatch(const Frame& request, bool& stop_after) {
  const CodecLimits limits = config_.codec;
  stop_after = false;
  Frame reply;
  switch (request.type) {
    case MessageType::Hello: {
      HelloRequest hello;
      const Status decoded = decode_request(request.body, hello, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      WelcomeReply welcome;
      welcome.protocol_version = LQF_WIRE_PROTOCOL_VERSION;
      welcome.server_epoch = fabric_->epoch();
      welcome.max_frame_bytes = config_.frames.max_frame_bytes;
      if (hello.protocol_version != LQF_WIRE_PROTOCOL_VERSION) {
        welcome.accepted = 0;
        welcome.detail = "protocol version mismatch";
      }
      reply.type = MessageType::Welcome;
      const Status encoded = encode_reply(welcome, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::RegisterCapability: {
      CapabilityDeclaration declaration;
      const Status decoded = decode_request(request.body, declaration, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<CapabilityAck> ack = fabric_->declare_capability(declaration);
      if (!ack.ok()) {
        return ack.status();
      }
      reply.type = MessageType::CapabilityAck;
      const Status encoded = encode_reply(ack.value(), reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::PublishPolicy: {
      PolicyDocument document;
      const Status decoded = decode_request(request.body, document, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<PolicyStamp> stamp = fabric_->publish_policy(document);
      if (!stamp.ok()) {
        return stamp.status();
      }
      PolicyAck ack;
      ack.stamp = stamp.value();
      reply.type = MessageType::PolicyAck;
      const Status encoded = encode_reply(ack, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Ingest: {
      std::vector<Observation> observations;
      const Status decoded = decode_request(request.body, observations, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<BatchOutcome> batch = fabric_->ingest_batch(observations);
      if (!batch.ok()) {
        return batch.status();
      }
      IngestAck ack;
      ack.accepted = batch.value().accepted;
      ack.duplicates = batch.value().duplicates;
      ack.reordered = batch.value().reordered;
      ack.rejected = batch.value().rejected;
      ack.first_code = batch.value().first_code;
      ack.first_reason = batch.value().first_reason;
      reply.type = MessageType::IngestAck;
      const Status encoded = encode_reply(ack, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Query: {
      QualityQuery query;
      const Status decoded = decode_request(request.body, query, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<LinkQualityReport> report = fabric_->query(query);
      if (!report.ok()) {
        return report.status();
      }
      reply.type = MessageType::QualityReport;
      const Status encoded = encode_reply(report.value(), reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Window: {
      WindowQuery query;
      const Status decoded = decode_request(request.body, query, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<WindowResult> result = fabric_->window(query);
      if (!result.ok()) {
        return result.status();
      }
      reply.type = MessageType::WindowReply;
      const Status encoded = encode_reply(result.value(), reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Explain: {
      QualityQuery query;
      const Status decoded = decode_request(request.body, query, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<Explanation> explanation = fabric_->explain(query);
      if (!explanation.ok()) {
        return explanation.status();
      }
      reply.type = MessageType::ExplanationReply;
      const Status encoded = encode_reply(explanation.value(), reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Inspect: {
      InspectionFilter filter;
      const Status decoded = decode_request(request.body, filter, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<InspectionReport> inspection = fabric_->inspect(filter);
      if (!inspection.ok()) {
        return inspection.status();
      }
      reply.type = MessageType::InspectionReply;
      const Status encoded = encode_reply(inspection.value(), reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Stats: {
      FabricStats fabric_stats = fabric_->stats();
      reply.type = MessageType::StatsReply;
      const Status encoded = encode_reply(fabric_stats, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Flush: {
      const Status flushed = fabric_->flush();
      if (!flushed.ok()) {
        // A runtime without persistence cannot report a successful flush: the
        // refusal crosses the boundary instead of being swallowed.
        return flushed;
      }
      FlushAck ack;
      const JournalStats journal = fabric_->journal_stats();
      ack.records_written = journal.records_written;
      ack.bytes_written = journal.bytes_written;
      reply.type = MessageType::FlushAck;
      const Status encoded = encode_reply(ack, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Compact: {
      const Status compacted = fabric_->compact();
      if (!compacted.ok()) {
        return compacted;
      }
      CompactAck ack;
      const JournalStats journal = fabric_->journal_stats();
      ack.records_written = journal.records_written;
      ack.compactions = journal.compactions;
      reply.type = MessageType::CompactAck;
      const Status encoded = encode_reply(ack, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Capabilities: {
      CapabilitiesRequest request_value;
      const Status decoded = decode_request(request.body, request_value, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      Outcome<std::vector<MetricCapabilityView>> views = fabric_->capabilities(request_value.link);
      if (!views.ok()) {
        return views.status();
      }
      CapabilitiesReply reply_value;
      reply_value.views = std::move(views.value());
      reply.type = MessageType::CapabilitiesReply;
      const Status encoded = encode_reply(reply_value, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::PolicyDocument: {
      PolicyDocumentRequest request_value;
      const Status decoded = decode_request(request.body, request_value, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      const PolicyGeneration generation = request_value.generation.is_zero()
                                             ? fabric_->current_policy().generation
                                             : request_value.generation;
      Outcome<PolicyGenerationRecord> record = fabric_->policy_document(generation);
      PolicyDocumentReply reply_value;
      if (record.ok()) {
        reply_value.record = std::move(record.value());
        reply_value.found = 1;
      } else {
        reply_value.found = 0;
        reply_value.detail = record.status().message();
      }
      reply.type = MessageType::PolicyDocumentReply;
      const Status encoded = encode_reply(reply_value, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      return reply;
    }
    case MessageType::Shutdown: {
      ShutdownRequest shutdown;
      const Status decoded = decode_request(request.body, shutdown, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      ShutdownAck ack;
      if (config_.shutdown_token.empty()) {
        ack.accepted = 0;
        ack.detail = "shutdown is not armed on this runtime";
        reply.type = MessageType::ShutdownAck;
        const Status encoded = encode_reply(ack, reply.body, limits);
        if (!encoded.ok()) {
          return encoded;
        }
        return reply;
      }
      if (shutdown.token != config_.shutdown_token) {
        return Status::error(StatusCode::Refused, "shutdown token does not match");
      }
      ack.accepted = 1;
      ack.detail = "shutdown accepted";
      reply.type = MessageType::ShutdownAck;
      const Status encoded = encode_reply(ack, reply.body, limits);
      if (!encoded.ok()) {
        return encoded;
      }
      stop_after = true;
      return reply;
    }
    default:
      return Status::error(StatusCode::Unsupported,
                           std::string("message type is not handled by this runtime: ") +
                               to_string(request.type));
  }
}

Status FabricServer::read_exact_interruptible(const Socket& socket, WakeupPair& wake, void* out,
                                              std::size_t size) {
  auto* bytes = static_cast<unsigned char*>(out);
  std::size_t received = 0;
  while (received < size) {
    if (stop_requested_.load()) {
      return Status::error(StatusCode::Cancelled, "the server is stopping");
    }
    bool socket_ready = false;
    bool wake_ready = false;
    const Status waited =
        Socket::wait_readable(socket.native(), wake.read_socket(), socket_ready, wake_ready);
    if (!waited.ok()) {
      return waited;
    }
    if (wake_ready) {
      const Status drained = wake.drain();
      (void)drained;
      continue;
    }
    if (!socket_ready) {
      continue;
    }
    const Outcome<std::size_t> chunk = socket.recv_some(bytes + received, size - received);
    if (!chunk.ok()) {
      return chunk.status();
    }
    if (chunk.value() == 0) {
      return Status::error(StatusCode::Cancelled, "peer closed the connection");
    }
    received += chunk.value();
  }
  return Status::success();
}

void FabricServer::connection_loop(Socket socket, std::shared_ptr<WakeupPair> wake,
                                   std::shared_ptr<std::atomic<bool>> done) {
  WakeupPair& channel = *wake;
  for (;;) {
    if (stop_requested_.load()) {
      break;
    }
    unsigned char header[kFrameHeaderBytes];
    const Status header_status =
        read_exact_interruptible(socket, channel, header, kFrameHeaderBytes);
    if (!header_status.ok()) {
      break;
    }
    u32 payload_length = 0;
    u32 payload_crc = 0;
    const Status decoded_header =
        decode_frame_header(header, config_.frames, payload_length, payload_crc);
    if (!decoded_header.ok()) {
      note_protocol_error();
      const Status sent = send_status(socket, decoded_header);
      (void)sent;
      break;
    }
    std::string payload(payload_length, '\0');
    const Status payload_status =
        read_exact_interruptible(socket, channel, payload.data(), payload_length);
    if (!payload_status.ok()) {
      break;
    }
    note_frames_in(kFrameHeaderBytes + payload_length);
    Frame request;
    const Status decoded_payload = decode_frame_payload(payload, payload_crc, request);
    if (!decoded_payload.ok()) {
      note_protocol_error();
      const Status sent = send_status(socket, decoded_payload);
      (void)sent;
      break;
    }

    bool stop_after = false;
    Outcome<Frame> reply = dispatch(request, stop_after);
    if (!reply.ok()) {
      const Status sent = send_status(socket, reply.status());
      if (!sent.ok()) {
        break;
      }
    } else {
      const Status sent = send_frame(socket, reply.value().type, std::move(reply.value().body));
      if (!sent.ok()) {
        break;
      }
    }
    if (stop_after) {
      const Status stopped = request_stop();
      (void)stopped;
      break;
    }
  }
  socket.shutdown_both();
  socket.close();
  done->store(true);
  active_connections_.fetch_sub(1);
}

Status FabricServer::request_stop() {
  stop_requested_.store(true);
  {
    locks::UniqueLock guard(stats_mutex_);
    stats_.stop_requested = true;
  }
  wait_cv_.notify_all();
  if (wake_.read_socket() != lqf_invalid_socket) {
    const Status woken = wake_.wake();
    if (!woken.ok()) {
      return woken;
    }
  }
  return Status::success();
}

void FabricServer::wait() {
  std::unique_lock<std::mutex> lock(wait_mutex_);
  wait_cv_.wait(lock, [this]() { return stop_requested_.load(); });
}

Status FabricServer::stop() {
  const bool was_running = running_.load() || accept_thread_.joinable();
  if (!was_running) {
    return Status::success();
  }
  const Status requested = request_stop();
  if (accept_thread_.joinable() && accept_thread_.get_id() != std::this_thread::get_id()) {
    accept_thread_.join();
  }
  {
    locks::UniqueLock guard(connections_mutex_);
    for (Connection& connection : connections_) {
      if (connection.thread.get_id() == std::this_thread::get_id()) {
        // Stopping the server from inside one of its own connection handlers
        // would make the join below a self join. It is refused instead.
        return Status::error(StatusCode::Refused,
                             "the server must be stopped from the owning thread, not from a "
                             "connection handler");
      }
    }
    for (Connection& connection : connections_) {
      // A real signal, not a deadline: the connection thread stops waiting,
      // re-checks the stop flag and closes its own socket. Issuing shutdown here
      // instead would leave a blocking recv that Windows does not reliably wake,
      // which turned a stop into a two minute stall.
      if (connection.wake != nullptr) {
        const Status woken = connection.wake->wake();
        (void)woken;
      }
    }
    for (Connection& connection : connections_) {
      if (connection.thread.joinable()) {
        connection.thread.join();
      }
    }
    connections_.clear();
  }
  wake_.close();
  listener_.close();
  {
    locks::UniqueLock guard(stats_mutex_);
    stats_.running = false;
    stats_.active_connections = active_connections_.load();
  }
  running_.store(false);
  return requested;
}

ServerStats FabricServer::stats() const {
  locks::UniqueLock guard(stats_mutex_);
  ServerStats copy = stats_;
  copy.active_connections = active_connections_.load();
  copy.stop_requested = stop_requested_.load();
  copy.running = running_.load();
  copy.bound_port = port_;
  return copy;
}

}  // namespace lqf
