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

#include "lqf/domain/quality.hpp"

#include <algorithm>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct StateName {
  QualityState state;
  const char* name;
  const char* description;
};

constexpr StateName kStateNames[] = {
    {QualityState::Unknown, "unknown",
     "no evidence has ever been seen for this identity, or no rule classifies the value"},
    {QualityState::Unsupported, "unsupported",
     "the metric family is not measured here; no value is synthesized"},
    {QualityState::Healthy, "healthy", "fresh evidence is inside every healthy band"},
    {QualityState::Incomplete, "incomplete",
     "evidence exists but cannot support a conclusion, for example broken counter continuity"},
    {QualityState::Stale, "stale", "evidence exists but nothing fresh enough to assert a state"},
    {QualityState::Marginal, "marginal", "fresh evidence is in a marginal band"},
    {QualityState::Degraded, "degraded", "fresh evidence is in a degraded band"},
    {QualityState::Severe, "severe", "fresh evidence is in a severe band"},
    {QualityState::Conflicting, "conflicting",
     "equal-authority sources disagree beyond the declared tolerance"},
};

struct CodeName {
  ReasonCode code;
  const char* name;
};

constexpr CodeName kReasonNames[] = {
    {ReasonCode::None, "none"},
    {ReasonCode::LinkNotRegistered, "link-not-registered"},
    {ReasonCode::NoSourcesForLink, "no-sources-for-link"},
    {ReasonCode::MetricNotDeclared, "metric-not-declared"},
    {ReasonCode::DeclaredUnsupported, "declared-unsupported"},
    {ReasonCode::LaneNotDeclared, "lane-not-declared"},
    {ReasonCode::LaneOutOfRange, "lane-out-of-range"},
    {ReasonCode::NoEvidence, "no-evidence"},
    {ReasonCode::NoRuleForMetric, "no-rule-for-metric"},
    {ReasonCode::UnknownMetric, "unknown-metric"},
    {ReasonCode::EvidenceExpired, "evidence-expired"},
    {ReasonCode::RecoveredEvidenceOnly, "recovered-evidence-only"},
    {ReasonCode::NoFreshEvidence, "no-fresh-evidence"},
    {ReasonCode::PartiallyFreshEvidence, "partially-fresh-evidence"},
    {ReasonCode::CounterBaselineOnly, "counter-baseline-only"},
    {ReasonCode::CounterReset, "counter-reset"},
    {ReasonCode::CounterWrapInferred, "counter-wrap-inferred"},
    {ReasonCode::CounterDecreaseAmbiguous, "counter-decrease-ambiguous"},
    {ReasonCode::CounterWidthChanged, "counter-width-changed"},
    {ReasonCode::CounterSourceReincarnated, "counter-source-reincarnated"},
    {ReasonCode::CounterLinkGenerationChanged, "counter-link-generation-changed"},
    {ReasonCode::CounterOutOfRange, "counter-out-of-range"},
    {ReasonCode::ContinuityGapExceeded, "continuity-gap-exceeded"},
    {ReasonCode::MissingSamples, "missing-samples"},
    {ReasonCode::RateIntervalTooShort, "rate-interval-too-short"},
    {ReasonCode::CounterSequenceRegressed, "counter-sequence-regressed"},
    {ReasonCode::EqualAuthorityDisagreement, "equal-authority-disagreement"},
    {ReasonCode::LowerAuthorityIgnored, "lower-authority-ignored"},
    {ReasonCode::MissingLanes, "missing-lanes"},
    {ReasonCode::LaneCoveragePartial, "lane-coverage-partial"},
    {ReasonCode::BandMatched, "band-matched"},
    {ReasonCode::PolicyDefaultApplied, "policy-default-applied"},
    {ReasonCode::NonFiniteSample, "non-finite-sample"},
    {ReasonCode::UnitMismatch, "unit-mismatch"},
    {ReasonCode::SemanticsMismatch, "semantics-mismatch"},
    {ReasonCode::NegativeValue, "negative-value"},
    {ReasonCode::CounterWidthMissing, "counter-width-missing"},
    {ReasonCode::EvidenceIdMismatch, "evidence-id-mismatch"},
    {ReasonCode::EvidenceTooLarge, "evidence-too-large"},
    {ReasonCode::CapabilityMissing, "capability-missing"},
    {ReasonCode::CapabilityScopeMismatch, "capability-scope-mismatch"},
    {ReasonCode::IngestLimitReached, "ingest-limit-reached"},
    {ReasonCode::QueueFull, "queue-full"},
    {ReasonCode::RecoveredFromPersistence, "recovered-from-persistence"},
    {ReasonCode::PersistenceDegraded, "persistence-degraded"},
    {ReasonCode::FencedGeneration, "fenced-generation"},
    {ReasonCode::FencedIncarnation, "fenced-incarnation"},
};

}  // namespace

const char* to_string(QualityState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.name;
    }
  }
  return "invalid";
}

std::string_view describe(QualityState state) noexcept {
  for (const StateName& entry : kStateNames) {
    if (entry.state == state) {
      return entry.description;
    }
  }
  return "unknown state";
}

Status parse_quality_state(std::string_view text_value, QualityState& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  for (const StateName& entry : kStateNames) {
    if (lowered == entry.name) {
      out = entry.state;
      return Status::success();
    }
  }
  return Status::error(StatusCode::Invalid, "unknown quality state: " + std::string(text_value));
}

const char* to_string(ConfidenceLevel level) noexcept {
  switch (level) {
    case ConfidenceLevel::None: return "none";
    case ConfidenceLevel::Low: return "low";
    case ConfidenceLevel::Medium: return "medium";
    case ConfidenceLevel::High: return "high";
  }
  return "invalid";
}

Status parse_confidence(std::string_view text_value, ConfidenceLevel& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  if (lowered == "none") {
    out = ConfidenceLevel::None;
  } else if (lowered == "low") {
    out = ConfidenceLevel::Low;
  } else if (lowered == "medium") {
    out = ConfidenceLevel::Medium;
  } else if (lowered == "high") {
    out = ConfidenceLevel::High;
  } else {
    return Status::error(StatusCode::Invalid, "unknown confidence level: " + std::string(text_value));
  }
  return Status::success();
}

const char* to_string(ReasonCode code) noexcept {
  for (const CodeName& entry : kReasonNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "invalid";
}

Status parse_reason_code(std::string_view text_value, ReasonCode& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  for (const CodeName& entry : kReasonNames) {
    if (lowered == entry.name) {
      out = entry.code;
      return Status::success();
    }
  }
  return Status::error(StatusCode::Invalid, "unknown reason code: " + std::string(text_value));
}

void add_reason(ReasonList& reasons, ReasonCode code) {
  if (code == ReasonCode::None) {
    return;
  }
  if (std::find(reasons.begin(), reasons.end(), code) == reasons.end()) {
    reasons.push_back(code);
  }
}

bool has_reason(const ReasonList& reasons, ReasonCode code) {
  return std::find(reasons.begin(), reasons.end(), code) != reasons.end();
}

std::string render_reasons(const ReasonList& reasons) {
  std::string out;
  bool first = true;
  for (const ReasonCode code : reasons) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    out.append(to_string(code));
  }
  if (out.empty()) {
    out = "none";
  }
  return out;
}

SeverityTable SeverityTable::defaults() {
  SeverityTable table;
  table.rank[static_cast<std::size_t>(QualityState::Unknown)] = 0;
  table.rank[static_cast<std::size_t>(QualityState::Unsupported)] = 1;
  table.rank[static_cast<std::size_t>(QualityState::Healthy)] = 2;
  table.rank[static_cast<std::size_t>(QualityState::Incomplete)] = 3;
  table.rank[static_cast<std::size_t>(QualityState::Stale)] = 4;
  table.rank[static_cast<std::size_t>(QualityState::Marginal)] = 5;
  table.rank[static_cast<std::size_t>(QualityState::Degraded)] = 6;
  table.rank[static_cast<std::size_t>(QualityState::Severe)] = 7;
  table.rank[static_cast<std::size_t>(QualityState::Conflicting)] = 8;
  return table;
}

bool SeverityTable::is_total_order() const noexcept {
  std::array<bool, kQualityStateCount> seen{};
  for (const u8 value : rank) {
    if (value >= kQualityStateCount || seen[value]) {
      return false;
    }
    seen[value] = true;
  }
  return true;
}

}  // namespace lqf
