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

#ifndef LQF_DOMAIN_IDENTITY_HPP
#define LQF_DOMAIN_IDENTITY_HPP

#include <optional>
#include <string>
#include <string_view>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/export.hpp"

namespace lqf {

// How the measurement reached this runtime. This is provenance, not a claim
// about hardware support: the runtime treats every transport identically.
enum class TransportKind : u8 {
  Unspecified = 0,
  Ethernet,
  Optical,
  Electrical,
  InternalFabric,
  Synthetic,
  Other,
  Count,
};

LQF_API const char* to_string(TransportKind kind) noexcept;
LQF_API Status parse_transport_kind(std::string_view text, TransportKind& out);

// What kind of evidence this is. The distinction is preserved end to end and
// surfaced in every report, so a synthetic generator can never be mistaken for
// a real measurement.
enum class EvidenceClass : u8 { Real = 0, Synthetic, Simulated, Count };

LQF_API const char* to_string(EvidenceClass value) noexcept;
LQF_API Status parse_evidence_class(std::string_view text, EvidenceClass& out);

// Whether a stored record was observed by this process incarnation or loaded
// from persistence. Recovered evidence is never fresh and can never assert a
// current state; it stays available for historical windows.
enum class EvidenceOrigin : u8 { Live = 0, Recovered, Count };

LQF_API const char* to_string(EvidenceOrigin origin) noexcept;

// A source is an identity plus an incarnation. A restarted source is a new
// incarnation: sequences restart, counter continuity is broken, and evidence
// from the previous incarnation can never be mixed into the new one.
struct SourceIdentity {
  SourceId id{};
  SourceIncarnation incarnation{SourceIncarnation(1)};

  friend bool operator==(const SourceIdentity&, const SourceIdentity&) = default;
  friend auto operator<=>(const SourceIdentity&, const SourceIdentity&) = default;
};

// A link is an identity plus a generation. Everything observed is scoped to the
// generation it was observed in, so a re-created link can never inherit the
// quality state of the link it replaced.
struct LinkIdentity {
  LinkId id{};
  LinkGeneration generation{LinkGeneration(1)};

  [[nodiscard]] bool empty() const noexcept { return id.is_empty(); }

  friend bool operator==(const LinkIdentity&, const LinkIdentity&) = default;
  friend auto operator<=>(const LinkIdentity&, const LinkIdentity&) = default;
};

struct Provenance {
  TransportKind transport{TransportKind::Unspecified};
  EvidenceClass evidence_class{EvidenceClass::Real};
  std::string origin{};    // where it came from, e.g. "host-a/nic0/port3"
  std::string producer{};  // which implementation produced it
  bool clock_synchronized{false};
  i64 declared_clock_offset_nanos{0};

  friend bool operator==(const Provenance&, const Provenance&) = default;
  friend auto operator<=>(const Provenance&, const Provenance&) = default;
};

LQF_API Status validate_source_identity(const SourceIdentity& identity);
LQF_API Status validate_link_identity(const LinkIdentity& identity);
LQF_API Status validate_provenance(const Provenance& provenance, std::size_t max_origin_bytes);

LQF_API std::string render_source_identity(const SourceIdentity& identity);
LQF_API std::string render_link_identity(const LinkIdentity& identity);

}  // namespace lqf

namespace std {

template <>
struct hash<lqf::SourceIdentity> {
  size_t operator()(const lqf::SourceIdentity& identity) const noexcept {
    return std::hash<std::string>{}(identity.id.value()) ^
           (std::hash<lqf::u64>{}(identity.incarnation.value()) << 1U);
  }
};

template <>
struct hash<lqf::LinkIdentity> {
  size_t operator()(const lqf::LinkIdentity& identity) const noexcept {
    return std::hash<std::string>{}(identity.id.value()) ^
           (std::hash<lqf::u64>{}(identity.generation.value()) << 1U);
  }
};

}  // namespace std

#endif  // LQF_DOMAIN_IDENTITY_HPP
