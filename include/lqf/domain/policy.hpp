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

#ifndef LQF_DOMAIN_POLICY_HPP
#define LQF_DOMAIN_POLICY_HPP

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/clock.hpp"
#include "lqf/core/lock_tracker.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/domain/counter.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/domain/quality.hpp"
#include "lqf/export.hpp"

namespace lqf {

// One classification band. Bands of a rule are ordered, contiguous and total
// over the extended reals, so every finite value classifies exactly once and a
// value can never fall through the policy.
struct ThresholdBand {
  RuleId id{};
  double lower{0.0};               // inclusive, may be -infinity
  std::optional<double> upper{};   // exclusive, absent means +infinity
  QualityState state{QualityState::Healthy};
  std::string label{};

  [[nodiscard]] bool contains(double value) const noexcept;
};

struct MetricRule {
  MetricId metric{};
  MetricBasis basis{MetricBasis::GaugeValue};
  std::vector<ThresholdBand> bands{};
  // Equal-authority readings that differ by more than this are CONFLICTING.
  // Zero means exact agreement is required; it never means "ignore".
  double conflict_tolerance{0.0};
  // Zero means "use the freshness default". Never means "never stale".
  i64 validity_nanos{0};
  i64 max_continuity_gap_nanos{0};
  // Modular wrap inference is opt-in and only applies when the source declared
  // a counter width.
  bool wrap_inference{false};
  double wrap_ceiling_fraction{0.75};
  double wrap_floor_fraction{0.25};

  [[nodiscard]] const ThresholdBand* classify(double value) const noexcept;
};

struct FreshnessPolicy {
  i64 default_validity_nanos{15'000'000'000LL};
  std::vector<std::pair<MetricFamily, i64>> family_validity_nanos{};
  i64 default_continuity_gap_nanos{60'000'000'000LL};
  i64 min_rate_interval_nanos{1'000'000LL};

  [[nodiscard]] i64 validity_for(MetricFamily family) const noexcept;
};

struct AggregationPolicy {
  SeverityTable severity{SeverityTable::defaults()};
  // When true, a CONFLICTING metric dominates every other state in the
  // link-wide roll-up. The default is true: disagreement is never hidden.
  bool conflicting_dominates{true};
};

struct PolicyDocument {
  PolicyId id{PolicyId(std::string("default"))};
  std::vector<MetricRule> rules{};
  FreshnessPolicy freshness{};
  AggregationPolicy aggregation{};
  std::vector<MetricId> unsupported_metrics{};
  // When true, evidence for a metric that no capability declaration covers is
  // rejected at ingest instead of being stored and reported as unsupported.
  bool require_capability{true};

  [[nodiscard]] const MetricRule* find_rule(const MetricId& metric) const noexcept;
};

struct PolicyStamp {
  PolicyId id{};
  PolicyGeneration generation{};
  std::string content_hash{};

  friend bool operator==(const PolicyStamp&, const PolicyStamp&) = default;
};

struct PolicyGenerationRecord {
  PolicyStamp stamp{};
  Timestamp published_at{};
  PolicyDocument document{};
};

LQF_API Status validate_policy(const PolicyDocument& document, const MetricCatalog& catalog,
                             std::size_t max_rules, std::size_t max_bands_per_rule);

// Deterministic canonical text of a document. The SHA-256 of this text is the
// published content hash, so two runs that publish the same policy produce the
// same generation content and classification provably cannot drift.
LQF_API std::string canonical_policy_text(const PolicyDocument& document);

LQF_API PolicyDocument default_policy_document();

// The continuity configuration a counter stream uses under a document. It is
// derived from the metric rule when one exists and from the freshness defaults
// otherwise, so live classification and historical reconstruction agree.
LQF_API CounterContinuityConfig counter_config_for(const PolicyDocument& document,
                                                 const MetricId& metric);

// Immutable, monotonically numbered policy generations. Publishing never
// rewrites raw evidence: it appends a generation, and every derived state names
// the generation that produced it.
class LQF_API PolicyRegistry {
 public:
  explicit PolicyRegistry(PolicyDocument initial, Timestamp published_at,
                          std::size_t max_generations = 64);

  Outcome<PolicyGenerationRecord> publish(const PolicyDocument& document, Timestamp published_at,
                                          const MetricCatalog& catalog, std::size_t max_rules,
                                          std::size_t max_bands_per_rule);

  // Installs a generation recovered from persistence with its original number.
  // A generation that is already present with the same content is a duplicate;
  // a re-used number with different content is an identity mismatch.
  Status restore(const PolicyGenerationRecord& record, const MetricCatalog& catalog,
                 std::size_t max_rules, std::size_t max_bands_per_rule);

  [[nodiscard]] PolicyGenerationRecord current() const;
  [[nodiscard]] std::optional<PolicyGenerationRecord> get(PolicyGeneration generation) const;
  [[nodiscard]] std::vector<PolicyStamp> history() const;
  [[nodiscard]] std::size_t generation_count() const;

 private:
  Status validate_and_install(const PolicyDocument& document, Timestamp published_at,
                              const MetricCatalog& catalog, std::size_t max_rules,
                              std::size_t max_bands_per_rule, PolicyGenerationRecord& out);

  mutable locks::TrackedMutex mutex_{"policy_registry"};
  std::vector<PolicyGenerationRecord> generations_{};
  std::size_t max_generations_{64};
};

}  // namespace lqf

namespace std {

template <>
struct hash<lqf::MetricId> {
  size_t operator()(const lqf::MetricId& id) const noexcept {
    return std::hash<std::string>{}(id.name) ^
           (std::hash<lqf::u8>{}(static_cast<lqf::u8>(id.family)) << 1U);
  }
};

template <>
struct hash<lqf::PolicyStamp> {
  size_t operator()(const lqf::PolicyStamp& stamp) const noexcept {
    return std::hash<std::string>{}(stamp.content_hash);
  }
};

}  // namespace std

#endif  // LQF_DOMAIN_POLICY_HPP
