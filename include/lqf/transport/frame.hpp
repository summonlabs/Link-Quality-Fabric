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

#ifndef LQF_TRANSPORT_FRAME_HPP
#define LQF_TRANSPORT_FRAME_HPP

#include <string>
#include <string_view>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Framing: [u32 payload length][u32 crc32c][u8 message type][body].
// The length is validated before anything is read into memory, so a peer can
// never ask this runtime to allocate an arbitrary buffer.
inline constexpr std::size_t kFrameHeaderBytes = 8;
inline constexpr u32 kMaxFrameBytesDefault = 1U << 20U;
inline constexpr u32 kMaxFrameBytesCeiling = 64U << 20U;

enum class MessageType : u8 {
  Invalid = 0,
  Hello = 1,
  RegisterCapability = 2,
  PublishPolicy = 3,
  Ingest = 4,
  Query = 5,
  Window = 6,
  Explain = 7,
  Inspect = 8,
  Stats = 9,
  Flush = 10,
  Compact = 11,
  Shutdown = 12,
  Capabilities = 13,
  PolicyDocument = 14,
  Welcome = 64,
  CapabilityAck = 65,
  PolicyAck = 66,
  IngestAck = 67,
  QualityReport = 68,
  WindowReply = 69,
  ExplanationReply = 70,
  InspectionReply = 71,
  StatsReply = 72,
  FlushAck = 73,
  CompactAck = 74,
  ShutdownAck = 75,
  CapabilitiesReply = 76,
  PolicyDocumentReply = 77,
  Error = 127,
};

LQF_API const char* to_string(MessageType type) noexcept;
LQF_API bool message_is_response(MessageType type) noexcept;

struct FrameLimits {
  u32 max_frame_bytes{kMaxFrameBytesDefault};
};

struct Frame {
  MessageType type{MessageType::Invalid};
  std::string body{};
};

LQF_API Status encode_frame(const Frame& frame, std::string& out, const FrameLimits& limits);

// Decodes the fixed header. The returned payload length excludes the header and
// the returned checksum covers the payload that must be read next.
LQF_API Status decode_frame_header(const u8* header, const FrameLimits& limits, u32& payload_length,
                                 u32& payload_crc);

// Decodes a payload whose length the caller already validated and whose
// checksum is verified before any field is interpreted.
LQF_API Status decode_frame_payload(std::string_view payload, u32 payload_crc, Frame& out);

// Error replies carry a status code and a bounded message; they are how a
// refusal crosses the boundary instead of becoming a silent success.
struct ErrorReply {
  StatusCode code{StatusCode::Internal};
  std::string message{};
};

LQF_API Status encode_error_reply(const ErrorReply& reply, std::string& out);
LQF_API Status decode_error_reply(std::string_view body, ErrorReply& out);

}  // namespace lqf

#endif  // LQF_TRANSPORT_FRAME_HPP
