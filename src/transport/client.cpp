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

#include "lqf/transport/client.hpp"

#include "lqf/core/text.hpp"
#include "lqf/persist/codec.hpp"
#include "lqf/version.hpp"

namespace lqf {

FabricClient::~FabricClient() { close(); }

Outcome<FabricClient> FabricClient::connect(const ClientConfig& config) {
  if (config.port == 0) {
    return Status::error(StatusCode::Invalid, "a port is required");
  }
  Outcome<Socket> socket = Socket::connect_loopback(config.port);
  if (!socket.ok()) {
    return socket.status();
  }
  FabricClient client;
  client.socket_ = std::move(socket.value());
  client.config_ = config;
  client.connected_ = true;
  return client;
}

void FabricClient::close() {
  if (socket_.valid()) {
    socket_.shutdown_both();
    socket_.close();
  }
  connected_ = false;
}

Status FabricClient::send_frame(const Frame& frame) {
  std::string encoded;
  const Status status = encode_frame(frame, encoded, config_.frames);
  if (!status.ok()) {
    return status;
  }
  return socket_.send_all(encoded);
}

Outcome<Frame> FabricClient::receive_frame() {
  unsigned char header[kFrameHeaderBytes];
  const Status header_status = socket_.recv_exact(header, kFrameHeaderBytes);
  if (!header_status.ok()) {
    return header_status;
  }
  u32 payload_length = 0;
  u32 payload_crc = 0;
  const Status decoded_header =
      decode_frame_header(header, config_.frames, payload_length, payload_crc);
  if (!decoded_header.ok()) {
    return decoded_header;
  }
  std::string payload(payload_length, '\0');
  const Status payload_status = socket_.recv_exact(payload.data(), payload_length);
  if (!payload_status.ok()) {
    return payload_status;
  }
  Frame frame;
  const Status decoded = decode_frame_payload(payload, payload_crc, frame);
  if (!decoded.ok()) {
    return decoded;
  }
  return frame;
}

Outcome<Frame> FabricClient::request(MessageType type, const std::string& body) {
  if (!connected_) {
    return Status::error(StatusCode::Unavailable, "the client is not connected");
  }
  Frame frame;
  frame.type = type;
  frame.body = body;
  const Status sent = send_frame(frame);
  if (!sent.ok()) {
    return sent;
  }
  Outcome<Frame> reply = receive_frame();
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type == MessageType::Error) {
    ErrorReply error;
    const Status decoded = decode_error_reply(reply.value().body, error);
    if (!decoded.ok()) {
      return decoded;
    }
    return Status::error(error.code, error.message);
  }
  if (!message_is_response(reply.value().type)) {
    return Status::error(StatusCode::Protocol,
                         std::string("peer replied with a request message: ") +
                             to_string(reply.value().type));
  }
  return reply;
}

Status FabricClient::handshake() {
  HelloRequest hello;
  hello.protocol_version = LQF_WIRE_PROTOCOL_VERSION;
  hello.client_name = config_.client_name;
  hello.client_epoch = config_.client_epoch;
  CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(hello);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Hello, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::Welcome) {
    return Status::error(StatusCode::Protocol, "handshake did not receive a welcome message");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  WelcomeReply welcome;
  decoder.field(welcome);
  if (!decoder.ok()) {
    return reader.status();
  }
  if (welcome.accepted == 0) {
    return Status::error(StatusCode::Unsupported,
                         "the runtime refused the connection: " + welcome.detail);
  }
  if (welcome.protocol_version != LQF_WIRE_PROTOCOL_VERSION) {
    return Status::error(StatusCode::Unsupported,
                         "the runtime speaks protocol version " +
                             text::format_u64(welcome.protocol_version));
  }
  server_epoch_ = welcome.server_epoch;
  return Status::success();
}

Outcome<CapabilityAck> FabricClient::declare_capability(const CapabilityDeclaration& declaration) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(declaration);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::RegisterCapability, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::CapabilityAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a capability declaration");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  CapabilityAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  return ack;
}

Outcome<PolicyStamp> FabricClient::publish_policy(const PolicyDocument& document) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(document);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::PublishPolicy, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::PolicyAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a policy publication");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  PolicyAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  return ack.stamp;
}

Outcome<BatchOutcome> FabricClient::ingest(const std::vector<Observation>& observations) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(observations);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Ingest, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::IngestAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to an ingest request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  IngestAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  BatchOutcome batch;
  batch.accepted = ack.accepted;
  batch.duplicates = ack.duplicates;
  batch.reordered = ack.reordered;
  batch.rejected = ack.rejected;
  batch.first_code = ack.first_code;
  batch.first_reason = ack.first_reason;
  return batch;
}

Outcome<LinkQualityReport> FabricClient::query(const QualityQuery& request_value) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(request_value);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Query, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::QualityReport) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a quality query");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  LinkQualityReport report;
  decoder.field(report);
  if (!decoder.ok()) {
    return reader.status();
  }
  return report;
}

Outcome<WindowResult> FabricClient::window(const WindowQuery& request_value) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(request_value);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Window, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::WindowReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a window query");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  WindowResult result;
  decoder.field(result);
  if (!decoder.ok()) {
    return reader.status();
  }
  return result;
}

Outcome<Explanation> FabricClient::explain(const QualityQuery& request_value) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(request_value);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Explain, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::ExplanationReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to an explanation request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  Explanation explanation;
  decoder.field(explanation);
  if (!decoder.ok()) {
    return reader.status();
  }
  return explanation;
}

Outcome<InspectionReport> FabricClient::inspect(const InspectionFilter& filter) {
  const CodecLimits limits = config_.codec;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(filter);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Inspect, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::InspectionReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to an inspection request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  InspectionReport report;
  decoder.field(report);
  if (!decoder.ok()) {
    return reader.status();
  }
  return report;
}

Outcome<std::vector<MetricCapabilityView>> FabricClient::capabilities(const LinkIdentity& link) {
  const CodecLimits limits = config_.codec;
  CapabilitiesRequest request_value;
  request_value.link = link;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(request_value);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Capabilities, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::CapabilitiesReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a capability query");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  CapabilitiesReply decoded;
  decoder.field(decoded);
  if (!decoder.ok()) {
    return reader.status();
  }
  return decoded.views;
}

Outcome<PolicyGenerationRecord> FabricClient::policy_document(PolicyGeneration generation) {
  const CodecLimits limits = config_.codec;
  PolicyDocumentRequest request_value;
  request_value.generation = generation;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(request_value);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::PolicyDocument, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::PolicyDocumentReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a policy document query");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  PolicyDocumentReply decoded;
  decoder.field(decoded);
  if (!decoder.ok()) {
    return reader.status();
  }
  if (decoded.found == 0) {
    return Status::error(StatusCode::NotFound,
                         decoded.detail.empty() ? "policy generation is not retained"
                                                : decoded.detail);
  }
  return decoded.record;
}

Outcome<FabricStats> FabricClient::stats() {
  const CodecLimits limits = config_.codec;
  Outcome<Frame> reply = request(MessageType::Stats, std::string());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::StatsReply) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a stats request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  FabricStats stats;
  decoder.field(stats);
  if (!decoder.ok()) {
    return reader.status();
  }
  return stats;
}

Outcome<FlushAck> FabricClient::flush() {
  const CodecLimits limits = config_.codec;
  Outcome<Frame> reply = request(MessageType::Flush, std::string());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::FlushAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a flush request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  FlushAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  return ack;
}

Outcome<CompactAck> FabricClient::compact() {
  const CodecLimits limits = config_.codec;
  Outcome<Frame> reply = request(MessageType::Compact, std::string());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::CompactAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a compaction request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  CompactAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  return ack;
}

Outcome<ShutdownAck> FabricClient::shutdown(const std::string& token) {
  const CodecLimits limits = config_.codec;
  ShutdownRequest shutdown_request;
  shutdown_request.token = token;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(shutdown_request);
  if (!encoder.ok()) {
    return writer.status();
  }
  Outcome<Frame> reply = request(MessageType::Shutdown, writer.buffer());
  if (!reply.ok()) {
    return reply.status();
  }
  if (reply.value().type != MessageType::ShutdownAck) {
    return Status::error(StatusCode::Protocol, "unexpected reply to a shutdown request");
  }
  Reader reader(reply.value().body, limits);
  Decoder decoder(reader, limits);
  ShutdownAck ack;
  decoder.field(ack);
  if (!decoder.ok()) {
    return reader.status();
  }
  return ack;
}

}  // namespace lqf
