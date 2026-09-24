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

#include "lqf/transport/frame.hpp"

#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"
#include "lqf/persist/archive.hpp"

namespace lqf {
namespace {

void encode_u32(unsigned char* out, u32 value) {
  out[0] = static_cast<unsigned char>(value & 0xFFU);
  out[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  out[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
  out[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

u32 decode_u32(const unsigned char* in) {
  return static_cast<u32>(in[0]) | (static_cast<u32>(in[1]) << 8U) |
         (static_cast<u32>(in[2]) << 16U) | (static_cast<u32>(in[3]) << 24U);
}

bool type_is_known(u8 raw) {
  switch (static_cast<MessageType>(raw)) {
    case MessageType::Hello:
    case MessageType::RegisterCapability:
    case MessageType::PublishPolicy:
    case MessageType::Ingest:
    case MessageType::Query:
    case MessageType::Window:
    case MessageType::Explain:
    case MessageType::Inspect:
    case MessageType::Stats:
    case MessageType::Flush:
    case MessageType::Compact:
    case MessageType::Shutdown:
    case MessageType::Capabilities:
    case MessageType::PolicyDocument:
    case MessageType::Welcome:
    case MessageType::CapabilityAck:
    case MessageType::PolicyAck:
    case MessageType::IngestAck:
    case MessageType::QualityReport:
    case MessageType::WindowReply:
    case MessageType::ExplanationReply:
    case MessageType::InspectionReply:
    case MessageType::StatsReply:
    case MessageType::FlushAck:
    case MessageType::CompactAck:
    case MessageType::ShutdownAck:
    case MessageType::CapabilitiesReply:
    case MessageType::PolicyDocumentReply:
    case MessageType::Error:
      return true;
    case MessageType::Invalid:
      return false;
  }
  return false;
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Invalid: return "invalid";
    case MessageType::Hello: return "hello";
    case MessageType::RegisterCapability: return "register-capability";
    case MessageType::PublishPolicy: return "publish-policy";
    case MessageType::Ingest: return "ingest";
    case MessageType::Query: return "query";
    case MessageType::Window: return "window";
    case MessageType::Explain: return "explain";
    case MessageType::Inspect: return "inspect";
    case MessageType::Stats: return "stats";
    case MessageType::Flush: return "flush";
    case MessageType::Compact: return "compact";
    case MessageType::Shutdown: return "shutdown";
    case MessageType::Capabilities: return "capabilities";
    case MessageType::PolicyDocument: return "policy-document";
    case MessageType::Welcome: return "welcome";
    case MessageType::CapabilityAck: return "capability-ack";
    case MessageType::PolicyAck: return "policy-ack";
    case MessageType::IngestAck: return "ingest-ack";
    case MessageType::QualityReport: return "quality-report";
    case MessageType::WindowReply: return "window-reply";
    case MessageType::ExplanationReply: return "explanation-reply";
    case MessageType::InspectionReply: return "inspection-reply";
    case MessageType::StatsReply: return "stats-reply";
    case MessageType::FlushAck: return "flush-ack";
    case MessageType::CompactAck: return "compact-ack";
    case MessageType::ShutdownAck: return "shutdown-ack";
    case MessageType::CapabilitiesReply: return "capabilities-reply";
    case MessageType::PolicyDocumentReply: return "policy-document-reply";
    case MessageType::Error: return "error";
  }
  return "invalid";
}

bool message_is_response(MessageType type) noexcept {
  return static_cast<u8>(type) >= static_cast<u8>(MessageType::Welcome);
}

Status encode_frame(const Frame& frame, std::string& out, const FrameLimits& limits) {
  if (frame.type == MessageType::Invalid || !type_is_known(static_cast<u8>(frame.type))) {
    return Status::error(StatusCode::Invalid, "message type is not recognised");
  }
  const u32 limit = limits.max_frame_bytes > kMaxFrameBytesCeiling ? kMaxFrameBytesCeiling
                                                                  : limits.max_frame_bytes;
  std::size_t payload_size = 0;
  if (!checked_add_size(frame.body.size(), 1, payload_size)) {
    return Status::error(StatusCode::Overflow, "frame size overflow");
  }
  if (payload_size > limit) {
    return Status::error(StatusCode::LimitExceeded,
                         "frame of " + std::to_string(payload_size) +
                             " bytes exceeds the limit of " + std::to_string(limit));
  }
  std::string payload;
  payload.reserve(payload_size);
  payload.push_back(static_cast<char>(frame.type));
  payload.append(frame.body);
  const u32 crc = crc32c(payload);
  out.resize(kFrameHeaderBytes);
  encode_u32(reinterpret_cast<unsigned char*>(out.data()), static_cast<u32>(payload.size()));
  encode_u32(reinterpret_cast<unsigned char*>(out.data()) + 4, crc);
  out.append(payload);
  return Status::success();
}

Status decode_frame_header(const u8* header, const FrameLimits& limits, u32& payload_length,
                           u32& payload_crc) {
  if (header == nullptr) {
    return Status::error(StatusCode::Internal, "frame header is null");
  }
  payload_length = decode_u32(header);
  payload_crc = decode_u32(header + 4);
  const u32 limit = limits.max_frame_bytes > kMaxFrameBytesCeiling ? kMaxFrameBytesCeiling
                                                                  : limits.max_frame_bytes;
  if (payload_length < 1) {
    return Status::error(StatusCode::Protocol, "frame length is below the minimum");
  }
  if (payload_length > limit) {
    return Status::error(StatusCode::Protocol,
                         "frame length of " + std::to_string(payload_length) +
                             " exceeds the limit of " + std::to_string(limit));
  }
  return Status::success();
}

Status decode_frame_payload(std::string_view payload, u32 payload_crc, Frame& out) {
  if (payload.empty()) {
    return Status::error(StatusCode::Protocol, "frame payload is empty");
  }
  if (crc32c(payload) != payload_crc) {
    return Status::error(StatusCode::Corrupt, "frame checksum does not match");
  }
  const u8 raw_type = static_cast<u8>(static_cast<unsigned char>(payload.front()));
  if (!type_is_known(raw_type)) {
    return Status::error(StatusCode::Protocol, "frame message type is not recognised");
  }
  out.type = static_cast<MessageType>(raw_type);
  out.body.assign(payload.substr(1));
  return Status::success();
}

Status encode_error_reply(const ErrorReply& reply, std::string& out) {
  CodecLimits limits;
  limits.max_string_bytes = 512;
  Writer writer(limits);
  writer.put_u8(static_cast<u8>(reply.code));
  writer.put_text(reply.message, limits.max_string_bytes);
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.release();
  return Status::success();
}

Status decode_error_reply(std::string_view body, ErrorReply& out) {
  CodecLimits limits;
  limits.max_string_bytes = 512;
  Reader reader(body, limits);
  u8 raw_code = 0;
  if (!reader.get_u8(raw_code)) {
    return reader.status();
  }
  if (raw_code > static_cast<u8>(StatusCode::Internal)) {
    return Status::error(StatusCode::Protocol, "error code is out of range");
  }
  out.code = static_cast<StatusCode>(raw_code);
  if (!reader.get_text(out.message, limits.max_string_bytes)) {
    return reader.status();
  }
  return Status::success();
}

}  // namespace lqf
