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

#ifndef LQF_DOMAIN_CLASSIFICATION_HPP
#define LQF_DOMAIN_CLASSIFICATION_HPP

#include <optional>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/domain/counter.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/domain/quality.hpp"
#include "lqf/export.hpp"

namespace lqf {

// One piece of evidence, summarised. Every derived state carries these so the
// conclusion can be audited down to the readings that produced it.
struct EvidenceRef {
  SourceIdentity source{};
  SequenceNumber sequence{};
  LaneDimension lane{};
  MetricId metric{};
  Timestamp observed_at{};
  ReceiveStamp received{};
  EvidenceOrigin origin{EvidenceOrigin::Live};
  EvidenceClass evidence_class{EvidenceClass::Real};
  AuthorityRank authority{};
  Unit unit{Unit::None};
  // Gauge value, or the rate a counter rule was applied to.
  double value{0.0};
  std::optional<u64> counter_reading{};
  std::optional<CounterDelta> delta{};
  bool used{false};       // this reading contributed to the state
  bool fresh{false};
  i64 age_nanos{0};       // steady-clock age for live evidence, wall age otherwise
  std::string rendered{}; // canonical text of the value
  std::string provenance{};  // where the reading came from, as declared by the source
};

struct MetricAssessment {
  MetricId metric{};
  LaneDimension lane{};
  QualityState state{QualityState::Unknown};
  ConfidenceLevel confidence{ConfidenceLevel::None};
  ReasonList reasons{};
  std::vector<EvidenceRef> evidence{};
  std::vector<RuleId> rules{};
  std::optional<double> value{};
  std::optional<double> rate_per_second{};
  std::optional<double> disagreement{};
  std::vector<LaneId> missing_lanes{};
  std::uint32_t sources_declared{0};
  std::uint32_t sources_with_evidence{0};
  std::uint32_t sources_fresh{0};
  std::uint32_t sources_conflicting{0};
  bool capability_declared{false};
  // Interval-average rate from a delta that spans more than the declared
  // continuity gap. It is reported, never used to assert current health.
  std::optional<double> interval_rate_per_second{};
};

struct LinkQualityReport {
  LinkIdentity link{};
  bool link_registered{false};
  bool evidence_present{false};
  PolicyStamp policy{};
  FabricEpoch epoch{};
  QualityState overall{QualityState::Unknown};
  ConfidenceLevel confidence{ConfidenceLevel::None};
  ReasonList reasons{};
  std::vector<MetricAssessment> metrics{};
  u64 evidence_considered{0};
  u64 evidence_fresh{0};
  u64 evidence_recovered{0};
  i64 generated_wall_nanos{0};
  i64 generated_steady_nanos{0};
  bool truncated{false};
};

struct QualityQuery {
  LinkIdentity link{};
  // Absent lane means the link-wide roll-up across every declared lane.
  std::optional<LaneDimension> lane{};
  // Empty means every metric declared by any source of this link.
  std::vector<MetricId> metrics{};
  bool include_evidence{true};
  std::size_t max_metrics{64};
  std::size_t max_evidence_per_metric{8};
};

enum class WindowAxis : u8 { ObservationTime = 0, ReceiveTime };

struct WindowQuery {
  std::optional<LinkIdentity> link{};
  std::optional<MetricId> metric{};
  std::optional<LaneDimension> lane{};
  std::optional<SourceId> source{};
  WindowAxis axis{WindowAxis::ObservationTime};
  i64 from_nanos{std::numeric_limits<i64>::min()};
  i64 to_nanos{std::numeric_limits<i64>::max()};
  std::size_t limit{1024};
  bool include_evidence{true};
  // Optional as-of assessment. Freshness is evaluated at as_of_steady_nanos,
  // which must be inside the current process incarnation; recovered evidence is
  // never fresh regardless of the instant chosen.
  bool assess_as_of{false};
  i64 as_of_steady_nanos{0};
  std::size_t max_metrics{64};
  std::size_t max_evidence_per_metric{8};
};

struct WindowResult {
  std::vector<EvidenceRecord> records{};
  u64 matched{0};
  u64 returned{0};
  bool truncated{false};
  i64 earliest_available_nanos{0};
  i64 latest_available_nanos{0};
  bool has_available_range{false};
  std::optional<LinkQualityReport> as_of_report{};
};

struct ConflictEntry {
  LinkIdentity link{};
  MetricId metric{};
  LaneDimension lane{};
  std::vector<EvidenceRef> claimants{};
  double spread{0.0};
  double tolerance{0.0};
};

struct StaleEntry {
  LinkIdentity link{};
  MetricId metric{};
  LaneDimension lane{};
  SourceIdentity source{};
  EvidenceOrigin origin{EvidenceOrigin::Live};
  i64 age_nanos{0};
  ReceiveStamp received{};
  ReasonCode reason{ReasonCode::EvidenceExpired};
};

struct FreshnessEntry {
  LinkIdentity link{};
  MetricId metric{};
  LaneDimension lane{};
  std::uint32_t fresh_sources{0};
  std::uint32_t stale_sources{0};
  std::uint32_t recovered_sources{0};
  std::uint32_t declared_sources{0};
  i64 newest_age_nanos{0};
  bool has_fresh_evidence{false};
};

struct InspectionFilter {
  std::optional<LinkIdentity> link{};
  std::optional<MetricId> metric{};
  std::size_t max_entries{256};
  std::size_t max_claimants_per_conflict{8};
};

struct InspectionReport {
  PolicyStamp policy{};
  FabricEpoch epoch{};
  i64 generated_wall_nanos{0};
  std::vector<ConflictEntry> conflicts{};
  std::vector<StaleEntry> stale{};
  std::vector<FreshnessEntry> freshness{};
  u64 links_inspected{0};
  u64 entries_dropped{0};
  bool truncated{false};
};

struct Explanation {
  LinkQualityReport report{};
  std::string text{};
};

// Deterministic renderings. Two runs over the same evidence and the same policy
// generation produce byte identical text.
LQF_API std::string render_quality_state(QualityState state);
LQF_API std::string render_report(const LinkQualityReport& report);
LQF_API std::string render_assessment(const MetricAssessment& assessment, std::size_t indent);
LQF_API std::string render_inspection(const InspectionReport& report);
LQF_API std::string render_evidence_record(const EvidenceRecord& record);

}  // namespace lqf

#endif  // LQF_DOMAIN_CLASSIFICATION_HPP
