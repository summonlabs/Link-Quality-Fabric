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

#ifndef LQF_DOMAIN_OBSERVATION_HPP
#define LQF_DOMAIN_OBSERVATION_HPP

#include <optional>
#include <string>
#include <variant>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/domain/identity.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/units.hpp"
#include "lqf/export.hpp"

namespace lqf {

// When the source says it happened, and whether that source's clock is
// synchronized to anything. An unsynchronized stamp is still evidence; it is
// simply never treated as comparable across sources.
struct Timestamp {
  i64 unix_nanos{0};
  bool synchronized{false};

  friend bool operator==(const Timestamp&, const Timestamp&) = default;
  friend auto operator<=>(const Timestamp&, const Timestamp&) = default;
};

// When this runtime received it. Every record carries the process incarnation
// that received it, which is what makes recovered evidence structurally
// incapable of becoming fresh.
struct ReceiveStamp {
  i64 wall_nanos{0};
  i64 steady_nanos{0};
  FabricEpoch epoch{};

  friend bool operator==(const ReceiveStamp&, const ReceiveStamp&) = default;
  friend auto operator<=>(const ReceiveStamp&, const ReceiveStamp&) = default;
};

struct GaugeReading {
  double value{0.0};
  // Declared validity window for this reading. Zero means "use the policy
  // default for this metric family"; it never means "valid forever".
  i64 validity_nanos{0};

  friend bool operator==(const GaugeReading&, const GaugeReading&) = default;
  friend auto operator<=>(const GaugeReading&, const GaugeReading&) = default;
};

struct CounterReading {
  u64 value{0};
  CounterWidth width{CounterWidth::Unspecified};
  // The source states that this reading follows a counter reset. A declared
  // reset re-baselines continuity; it never becomes a delta.
  bool reset_declared{false};

  friend bool operator==(const CounterReading&, const CounterReading&) = default;
};

using Reading = std::variant<GaugeReading, CounterReading>;

[[nodiscard]] inline bool is_gauge(const Reading& reading) noexcept {
  return std::holds_alternative<GaugeReading>(reading);
}
[[nodiscard]] inline bool is_counter(const Reading& reading) noexcept {
  return std::holds_alternative<CounterReading>(reading);
}
[[nodiscard]] inline const GaugeReading* as_gauge(const Reading& reading) noexcept {
  return std::get_if<GaugeReading>(&reading);
}
[[nodiscard]] inline const CounterReading* as_counter(const Reading& reading) noexcept {
  return std::get_if<CounterReading>(&reading);
}

// The identity of one evidence stream: one link generation, one metric, one
// lane dimension and one source incarnation. Streams never merge across
// incarnations, which is what makes reincarnation fencing structural.
struct StreamKey {
  LinkIdentity link{};
  MetricId metric{};
  LaneDimension lane{};
  SourceIdentity source{};

  friend bool operator==(const StreamKey&, const StreamKey&) = default;
  friend auto operator<=>(const StreamKey&, const StreamKey&) = default;
};

struct ObservationLimits {
  std::size_t max_origin_bytes{96};
  std::size_t max_producer_bytes{64};
  std::uint32_t max_lanes{256};
};

// A raw measurement. This is not a diagnosis: nothing here is a quality state,
// and nothing here is derived from another observation.
struct Observation {
  LinkIdentity link{};
  SourceIdentity source{};
  SequenceNumber sequence{};
  MetricId metric{};
  LaneDimension lane{};
  Unit unit{Unit::None};
  Reading reading{GaugeReading{}};
  Timestamp observed_at{};
  std::optional<i64> interval_nanos{};
  AuthorityRank authority{AuthorityRank(1)};
  Provenance provenance{};

  [[nodiscard]] SampleSemantics semantics() const noexcept {
    return is_gauge(reading) ? SampleSemantics::Gauge : SampleSemantics::Counter;
  }

  friend bool operator==(const Observation&, const Observation&) = default;
};

// A stored observation. The runtime adds the receive stamp, the monotonic
// ingest ordinal and the origin. The source supplied times are never rewritten.
struct EvidenceRecord {
  Observation observation{};
  ReceiveStamp received{};
  IngestOrdinal ordinal{};
  EvidenceOrigin origin{EvidenceOrigin::Live};

  friend bool operator==(const EvidenceRecord&, const EvidenceRecord&) = default;
};

// Validates everything that can be checked without knowing the declared
// capability. The descriptor is optional: when present, unit and semantics must
// agree with it exactly.
LQF_API Status validate_observation(const Observation& observation,
                                   const MetricDescriptor* descriptor,
                                   const ObservationLimits& limits);

// Canonical rendering of a reading. Used by explanations, the CLI and digests,
// so it must stay deterministic and locale independent.
LQF_API std::string render_reading(const Reading& reading, Unit unit);

}  // namespace lqf

#endif  // LQF_DOMAIN_OBSERVATION_HPP
