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

#include "lqf/domain/identity.hpp"

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct KindName {
  TransportKind kind;
  const char* name;
};

constexpr KindName kTransportNames[] = {
    {TransportKind::Unspecified, "unspecified"}, {TransportKind::Ethernet, "ethernet"},
    {TransportKind::Optical, "optical"},         {TransportKind::Electrical, "electrical"},
    {TransportKind::InternalFabric, "internal-fabric"},
    {TransportKind::Synthetic, "synthetic"},     {TransportKind::Other, "other"},
};

}  // namespace

const char* to_string(TransportKind kind) noexcept {
  for (const KindName& entry : kTransportNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "invalid";
}

Status parse_transport_kind(std::string_view text_value, TransportKind& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  for (const KindName& entry : kTransportNames) {
    if (lowered == entry.name) {
      out = entry.kind;
      return Status::success();
    }
  }
  return Status::error(StatusCode::Invalid, "unknown transport kind: " + std::string(text_value));
}

const char* to_string(EvidenceClass value) noexcept {
  switch (value) {
    case EvidenceClass::Real: return "real";
    case EvidenceClass::Synthetic: return "synthetic";
    case EvidenceClass::Simulated: return "simulated";
    case EvidenceClass::Count: break;
  }
  return "invalid";
}

Status parse_evidence_class(std::string_view text_value, EvidenceClass& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  if (lowered == "real") {
    out = EvidenceClass::Real;
  } else if (lowered == "synthetic") {
    out = EvidenceClass::Synthetic;
  } else if (lowered == "simulated") {
    out = EvidenceClass::Simulated;
  } else {
    return Status::error(StatusCode::Invalid, "unknown evidence class: " + std::string(text_value));
  }
  return Status::success();
}

const char* to_string(EvidenceOrigin origin) noexcept {
  switch (origin) {
    case EvidenceOrigin::Live: return "live";
    case EvidenceOrigin::Recovered: return "recovered";
    case EvidenceOrigin::Count: break;
  }
  return "invalid";
}

Status validate_identity_text(std::string_view value, std::size_t max_bytes, const char* what) {
  if (value.empty()) {
    return Status::error(StatusCode::Invalid, std::string(what) + " must not be empty");
  }
  if (value.size() > max_bytes) {
    return Status::error(StatusCode::Invalid,
                         std::string(what) + " exceeds " + std::to_string(max_bytes) + " bytes");
  }
  for (const char character : value) {
    const auto raw = static_cast<unsigned char>(character);
    if (raw < 0x21U || raw > 0x7EU) {
      return Status::error(StatusCode::Invalid,
                           std::string(what) + " must be printable ASCII without spaces");
    }
  }
  return Status::success();
}

Status validate_lane_id(std::uint32_t lane, std::uint32_t max_lanes) {
  if (max_lanes == 0) {
    return Status::error(StatusCode::Invalid, "lane capacity is zero");
  }
  if (lane >= max_lanes) {
    return Status::error(StatusCode::Invalid,
                         "lane " + std::to_string(lane) + " exceeds the declared capacity of " +
                             std::to_string(max_lanes));
  }
  return Status::success();
}

Status validate_source_identity(const SourceIdentity& identity) {
  const Status id_status =
      validate_identity_text(identity.id.value(), kMaxIdentityBytes, "source id");
  if (!id_status.ok()) {
    return id_status;
  }
  if (identity.incarnation.is_zero()) {
    return Status::error(StatusCode::Invalid, "source incarnation must be at least 1");
  }
  return Status::success();
}

Status validate_link_identity(const LinkIdentity& identity) {
  const Status id_status = validate_identity_text(identity.id.value(), kMaxIdentityBytes, "link id");
  if (!id_status.ok()) {
    return id_status;
  }
  if (identity.generation.is_zero()) {
    return Status::error(StatusCode::Invalid, "link generation must be at least 1");
  }
  return Status::success();
}

Status validate_provenance(const Provenance& provenance, std::size_t max_origin_bytes) {
  if (provenance.transport == TransportKind::Count) {
    return Status::error(StatusCode::Invalid, "transport kind is out of range");
  }
  if (provenance.evidence_class == EvidenceClass::Count) {
    return Status::error(StatusCode::Invalid, "evidence class is out of range");
  }
  if (!provenance.origin.empty()) {
    const Status origin_status =
        validate_identity_text(provenance.origin, max_origin_bytes, "provenance origin");
    if (!origin_status.ok()) {
      return origin_status;
    }
  }
  if (!provenance.producer.empty()) {
    const Status producer_status =
        validate_identity_text(provenance.producer, max_origin_bytes, "provenance producer");
    if (!producer_status.ok()) {
      return producer_status;
    }
  }
  return Status::success();
}

std::string render_source_identity(const SourceIdentity& identity) {
  std::string out = identity.id.value();
  out.push_back('@');
  out.append(text::format_u64(identity.incarnation.value()));
  return out;
}

std::string render_link_identity(const LinkIdentity& identity) {
  std::string out = identity.id.value();
  out.append("/gen");
  out.append(text::format_u64(identity.generation.value()));
  return out;
}

}  // namespace lqf
