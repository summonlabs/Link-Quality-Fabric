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

#ifndef LQF_TRANSPORT_MESSAGES_HPP
#define LQF_TRANSPORT_MESSAGES_HPP

#include <string>
#include <vector>

#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/persist/codec.hpp"
#include "lqf/runtime/config.hpp"
#include "lqf/transport/frame.hpp"

namespace lqf {

struct HelloRequest {
  u32 protocol_version{1};
  std::string client_name{};
  u64 client_epoch{0};
};

struct WelcomeReply {
  u32 protocol_version{1};
  FabricEpoch server_epoch{};
  u32 max_frame_bytes{kMaxFrameBytesDefault};
  u8 accepted{1};
  std::string detail{};
};

struct CapabilityAck {
  CapabilityRevision revision{CapabilityRevision(1)};
  ReasonCode reason{ReasonCode::None};
  u8 duplicate{0};
};

struct PolicyAck {
  PolicyStamp stamp{};
};

struct IngestAck {
  u64 accepted{0};
  u64 duplicates{0};
  u64 reordered{0};
  u64 rejected{0};
  StatusCode first_code{StatusCode::Ok};
  ReasonCode first_reason{ReasonCode::None};
};

struct FlushAck {
  u64 records_written{0};
  u64 bytes_written{0};
};

struct CompactAck {
  u64 records_written{0};
  u64 compactions{0};
};

struct ShutdownRequest {
  std::string token{};
};

struct ShutdownAck {
  u8 accepted{0};
  std::string detail{};
};

struct CapabilitiesRequest {
  LinkIdentity link{};
};

struct CapabilitiesReply {
  std::vector<MetricCapabilityView> views{};
};

struct PolicyDocumentRequest {
  // Zero means "the current generation".
  PolicyGeneration generation{};
};

struct PolicyDocumentReply {
  PolicyGenerationRecord record{};
  u8 found{0};
  std::string detail{};
};

struct RecoverySummary {
  bool opened{false};
  bool header_valid{false};
  bool torn_tail{false};
  bool degraded{false};
  bool truncated{false};
  bool snapshot_loaded{false};
  bool limit_reached{false};
  u64 records_read{0};
  u64 bytes_read{0};
  u64 bytes_discarded{0};
  StatusCode status{StatusCode::Ok};
};

template <class Archive>
void visit_fields(Archive& archive, HelloRequest& value) {
  archive.field(value.protocol_version);
  archive.field(value.client_name);
  archive.field(value.client_epoch);
}

template <class Archive>
void visit_fields(Archive& archive, WelcomeReply& value) {
  archive.field(value.protocol_version);
  archive.field(value.server_epoch);
  archive.field(value.max_frame_bytes);
  archive.field(value.accepted);
  archive.field(value.detail);
}

template <class Archive>
void visit_fields(Archive& archive, CapabilityAck& value) {
  archive.field(value.revision);
  archive.field(value.reason);
  archive.field(value.duplicate);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyAck& value) {
  archive.field(value.stamp);
}

template <class Archive>
void visit_fields(Archive& archive, IngestAck& value) {
  archive.field(value.accepted);
  archive.field(value.duplicates);
  archive.field(value.reordered);
  archive.field(value.rejected);
  archive.field(value.first_code);
  archive.field(value.first_reason);
}

template <class Archive>
void visit_fields(Archive& archive, FlushAck& value) {
  archive.field(value.records_written);
  archive.field(value.bytes_written);
}

template <class Archive>
void visit_fields(Archive& archive, CompactAck& value) {
  archive.field(value.records_written);
  archive.field(value.compactions);
}

template <class Archive>
void visit_fields(Archive& archive, ShutdownRequest& value) {
  archive.field(value.token);
}

template <class Archive>
void visit_fields(Archive& archive, ShutdownAck& value) {
  archive.field(value.accepted);
  archive.field(value.detail);
}

template <class Archive>
void visit_fields(Archive& archive, CapabilitiesRequest& value) {
  archive.field(value.link);
}

template <class Archive>
void visit_fields(Archive& archive, CapabilitiesReply& value) {
  archive.field(value.views);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyDocumentRequest& value) {
  archive.field(value.generation);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyDocumentReply& value) {
  archive.field(value.record);
  archive.field(value.found);
  archive.field(value.detail);
}

template <class Archive>
void visit_fields(Archive& archive, RecoverySummary& value) {
  archive.field(value.opened);
  archive.field(value.header_valid);
  archive.field(value.torn_tail);
  archive.field(value.degraded);
  archive.field(value.truncated);
  archive.field(value.snapshot_loaded);
  archive.field(value.limit_reached);
  archive.field(value.records_read);
  archive.field(value.bytes_read);
  archive.field(value.bytes_discarded);
  archive.field(value.status);
}

}  // namespace lqf

#endif  // LQF_TRANSPORT_MESSAGES_HPP
