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

#ifndef LQF_DOMAIN_QUALITY_HPP
#define LQF_DOMAIN_QUALITY_HPP

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/export.hpp"

namespace lqf {

// The derived quality vocabulary. A quality state is a conclusion about
// evidence, never a measurement, and never a binary up/down verdict.
//
//   Healthy      - fresh evidence exists and every declared metric is inside
//                  its healthy band.
//   Marginal     - fresh evidence exists and at least one metric is in a
//                  marginal band. Usable, trending, not yet degraded.
//   Degraded     - fresh evidence exists and at least one metric is in a
//                  degraded band.
//   Severe       - fresh evidence exists and at least one metric is in a
//                  severe band.
//   Unknown      - no evidence has ever been seen for the requested identity,
//                  or no rule classifies the value. Absence of evidence is
//                  never health and is never zero.
//   Stale        - evidence exists but nothing fresh enough to assert a current
//                  state, including anything recovered from persistence.
//   Conflicting  - equal-authority sources disagree beyond the declared
//                  tolerance. Both provenances are preserved; nothing is
//                  averaged into false certainty.
//   Incomplete   - evidence exists but cannot support a conclusion: counter
//                  continuity is broken, a lane is missing, or only one of the
//                  two readings a rate needs is fresh.
//   Unsupported  - the metric family is not measured here. Declared or not
//                  declared, it stays unsupported; no value is synthesized.
enum class QualityState : u8 {
  Unknown = 0,
  Unsupported,
  Healthy,
  Incomplete,
  Stale,
  Marginal,
  Degraded,
  Severe,
  Conflicting,
};

inline constexpr std::size_t kQualityStateCount = 9;

LQF_API const char* to_string(QualityState state) noexcept;
LQF_API std::string_view describe(QualityState state) noexcept;
LQF_API Status parse_quality_state(std::string_view text, QualityState& out);

// How much the conclusion is worth. Confidence is about agreement, freshness
// and freshness margin; it is never a claim about the physical link.
enum class ConfidenceLevel : u8 { None = 0, Low, Medium, High };

LQF_API const char* to_string(ConfidenceLevel level) noexcept;
LQF_API Status parse_confidence(std::string_view text, ConfidenceLevel& out);

// Machine-readable explanation codes. Every derived state carries the codes
// that produced it, so a state can always be explained without re-deriving it.
enum class ReasonCode : u16 {
  None = 0,

  // Availability and capability.
  LinkNotRegistered,
  NoSourcesForLink,
  MetricNotDeclared,
  DeclaredUnsupported,
  LaneNotDeclared,
  LaneOutOfRange,
  NoEvidence,
  NoRuleForMetric,
  UnknownMetric,

  // Freshness.
  EvidenceExpired,
  RecoveredEvidenceOnly,
  NoFreshEvidence,
  PartiallyFreshEvidence,

  // Counter continuity.
  CounterBaselineOnly,
  CounterReset,
  CounterWrapInferred,
  CounterDecreaseAmbiguous,
  CounterWidthChanged,
  CounterSourceReincarnated,
  CounterLinkGenerationChanged,
  CounterOutOfRange,
  ContinuityGapExceeded,
  MissingSamples,
  RateIntervalTooShort,
  CounterSequenceRegressed,

  // Conflict and authority.
  EqualAuthorityDisagreement,
  LowerAuthorityIgnored,

  // Completeness.
  MissingLanes,
  LaneCoveragePartial,

  // Policy.
  BandMatched,
  PolicyDefaultApplied,

  // Validation and rejection.
  NonFiniteSample,
  UnitMismatch,
  SemanticsMismatch,
  NegativeValue,
  CounterWidthMissing,
  EvidenceIdMismatch,
  EvidenceTooLarge,
  CapabilityMissing,
  CapabilityScopeMismatch,

  // Runtime and persistence.
  IngestLimitReached,
  QueueFull,
  RecoveredFromPersistence,
  PersistenceDegraded,
  FencedGeneration,
  FencedIncarnation,

  Count,
};

LQF_API const char* to_string(ReasonCode code) noexcept;
LQF_API Status parse_reason_code(std::string_view text, ReasonCode& out);

using ReasonList = std::vector<ReasonCode>;
LQF_API void add_reason(ReasonList& reasons, ReasonCode code);
LQF_API bool has_reason(const ReasonList& reasons, ReasonCode code);
LQF_API std::string render_reasons(const ReasonList& reasons);

// Severity ranks are policy, not code: an operator can declare that a stale
// link outranks a degraded one. The default table is documented in the README
// and validated to be a total order over every quality state.
struct SeverityTable {
  std::array<u8, kQualityStateCount> rank{};

  [[nodiscard]] u8 of(QualityState state) const noexcept { return rank[static_cast<std::size_t>(state)]; }
  [[nodiscard]] static SeverityTable defaults();
  [[nodiscard]] bool is_total_order() const noexcept;
  [[nodiscard]] QualityState worst(QualityState lhs, QualityState rhs) const noexcept {
    return of(lhs) >= of(rhs) ? lhs : rhs;
  }
};

}  // namespace lqf

#endif  // LQF_DOMAIN_QUALITY_HPP
