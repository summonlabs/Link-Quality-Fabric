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

#include "lqf/domain/policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"

namespace lqf {
namespace {

constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();
constexpr double kPositiveInfinity = std::numeric_limits<double>::infinity();

bool state_is_valid(QualityState state) { return state <= QualityState::Conflicting; }

void append_double(std::string& out, double value) {
  if (std::isinf(value)) {
    out.append(value > 0 ? "+inf" : "-inf");
    return;
  }
  if (std::isnan(value)) {
    out.append("nan");
    return;
  }
  out.append(text::format_double(value));
}

Status validate_bands(const MetricRule& rule, std::size_t max_bands) {
  if (rule.bands.empty()) {
    return Status::error(StatusCode::Invalid,
                         "rule has no bands: " + render_metric_id(rule.metric));
  }
  if (rule.bands.size() > max_bands) {
    return Status::error(StatusCode::LimitExceeded,
                         "rule exceeds the band limit: " + render_metric_id(rule.metric));
  }
  std::set<std::string> ids;
  for (std::size_t index = 0; index < rule.bands.size(); ++index) {
    const ThresholdBand& band = rule.bands[index];
    if (band.id.is_empty()) {
      return Status::error(StatusCode::Invalid,
                           "band id must not be empty: " + render_metric_id(rule.metric));
    }
    if (!ids.insert(band.id.value()).second) {
      return Status::error(StatusCode::Invalid,
                           "duplicate band id: " + band.id.value());
    }
    if (!state_is_valid(band.state)) {
      return Status::error(StatusCode::Invalid,
                           "band state is out of range: " + band.id.value());
    }
    if (std::isnan(band.lower)) {
      return Status::error(StatusCode::Invalid, "band lower bound is NaN: " + band.id.value());
    }
    if (band.upper.has_value() && std::isnan(*band.upper)) {
      return Status::error(StatusCode::Invalid, "band upper bound is NaN: " + band.id.value());
    }
    if (band.upper.has_value() && !(*band.upper > band.lower)) {
      return Status::error(StatusCode::Invalid,
                           "band upper bound is not above its lower bound: " + band.id.value());
    }
    if (index == 0) {
      if (band.lower != kNegativeInfinity) {
        return Status::error(StatusCode::Invalid,
                             "the first band must start at -infinity so that every value "
                             "classifies exactly once: " +
                                 render_metric_id(rule.metric));
      }
      continue;
    }
    const ThresholdBand& previous = rule.bands[index - 1];
    if (!previous.upper.has_value()) {
      return Status::error(StatusCode::Invalid,
                           "band before the last one must have an upper bound: " +
                               previous.id.value());
    }
    if (*previous.upper != band.lower) {
      return Status::error(StatusCode::Invalid,
                           "bands are not contiguous at " + previous.id.value() + " -> " +
                               band.id.value() + "; gaps would leave values unclassified");
    }
  }
  if (rule.bands.back().upper.has_value()) {
    return Status::error(StatusCode::Invalid,
                         "the last band must extend to +infinity so that every value classifies "
                         "exactly once: " +
                             render_metric_id(rule.metric));
  }
  return Status::success();
}

bool metric_in_list(const std::vector<MetricId>& metrics, const MetricId& metric) {
  return std::find(metrics.begin(), metrics.end(), metric) != metrics.end();
}

}  // namespace

bool ThresholdBand::contains(double value) const noexcept {
  if (std::isnan(value)) {
    return false;
  }
  if (value < lower) {
    return false;
  }
  if (upper.has_value() && !(value < *upper)) {
    return false;
  }
  return true;
}

const ThresholdBand* MetricRule::classify(double value) const noexcept {
  if (std::isnan(value)) {
    return nullptr;
  }
  for (const ThresholdBand& band : bands) {
    if (band.contains(value)) {
      return &band;
    }
  }
  return nullptr;
}

i64 FreshnessPolicy::validity_for(MetricFamily family) const noexcept {
  for (const auto& entry : family_validity_nanos) {
    if (entry.first == family) {
      return entry.second;
    }
  }
  return default_validity_nanos;
}

const MetricRule* PolicyDocument::find_rule(const MetricId& metric) const noexcept {
  for (const MetricRule& rule : rules) {
    if (rule.metric == metric) {
      return &rule;
    }
  }
  return nullptr;
}

Status validate_policy(const PolicyDocument& document, const MetricCatalog& catalog,
                       std::size_t max_rules, std::size_t max_bands_per_rule) {
  Status status = validate_identity_text(document.id.value(), kMaxIdentityBytes, "policy id");
  if (!status.ok()) {
    return status;
  }
  if (document.rules.size() > max_rules) {
    return Status::error(StatusCode::LimitExceeded,
                         "policy exceeds the rule limit of " + std::to_string(max_rules));
  }
  std::set<MetricId> rule_metrics;
  for (const MetricRule& rule : document.rules) {
    status = validate_metric_id(rule.metric);
    if (!status.ok()) {
      return status;
    }
    if (!rule_metrics.insert(rule.metric).second) {
      return Status::error(StatusCode::Invalid,
                           "duplicate rule for metric: " + render_metric_id(rule.metric));
    }
    const MetricDescriptor* descriptor = catalog.find(rule.metric);
    if (descriptor == nullptr) {
      return Status::error(StatusCode::Unsupported,
                           "rule references a metric that is not in the catalog: " +
                               render_metric_id(rule.metric));
    }
    if (descriptor->semantics == SampleSemantics::Gauge && rule.basis != MetricBasis::GaugeValue) {
      return Status::error(StatusCode::Invalid,
                           "gauge metric must be classified by gauge value: " +
                               render_metric_id(rule.metric));
    }
    if (descriptor->semantics == SampleSemantics::Counter && rule.basis == MetricBasis::GaugeValue) {
      return Status::error(StatusCode::Invalid,
                           "counter metric must be classified by a counter basis: " +
                               render_metric_id(rule.metric));
    }
    if (!(rule.conflict_tolerance >= 0.0) || !std::isfinite(rule.conflict_tolerance)) {
      return Status::error(StatusCode::Invalid,
                           "conflict tolerance must be finite and non-negative: " +
                               render_metric_id(rule.metric));
    }
    if (rule.validity_nanos < 0) {
      return Status::error(StatusCode::Invalid,
                           "rule validity window must not be negative: " +
                               render_metric_id(rule.metric));
    }
    if (rule.max_continuity_gap_nanos < 0) {
      return Status::error(StatusCode::Invalid,
                           "rule continuity gap must not be negative: " +
                               render_metric_id(rule.metric));
    }
    if (!(rule.wrap_ceiling_fraction >= 0.0) || !(rule.wrap_ceiling_fraction <= 1.0) ||
        !(rule.wrap_floor_fraction >= 0.0) || !(rule.wrap_floor_fraction <= 1.0)) {
      return Status::error(StatusCode::Invalid,
                           "wrap fractions must be within [0,1]: " +
                               render_metric_id(rule.metric));
    }
    if (rule.wrap_floor_fraction >= rule.wrap_ceiling_fraction) {
      return Status::error(StatusCode::Invalid,
                           "wrap floor fraction must be below the ceiling fraction: " +
                               render_metric_id(rule.metric));
    }
    status = validate_bands(rule, max_bands_per_rule);
    if (!status.ok()) {
      return status;
    }
  }

  std::set<MetricId> unsupported;
  for (const MetricId& metric : document.unsupported_metrics) {
    status = validate_metric_id(metric);
    if (!status.ok()) {
      return status;
    }
    if (!unsupported.insert(metric).second) {
      return Status::error(StatusCode::Invalid,
                           "duplicate unsupported metric: " + render_metric_id(metric));
    }
    if (rule_metrics.find(metric) != rule_metrics.end()) {
      return Status::error(StatusCode::Conflict,
                           "metric is both unsupported and classified: " +
                               render_metric_id(metric));
    }
  }

  if (document.freshness.default_validity_nanos <= 0) {
    return Status::error(StatusCode::Invalid, "default validity window must be positive");
  }
  if (document.freshness.default_continuity_gap_nanos <= 0) {
    return Status::error(StatusCode::Invalid, "default continuity gap must be positive");
  }
  if (document.freshness.min_rate_interval_nanos < 0) {
    return Status::error(StatusCode::Invalid, "minimum rate interval must not be negative");
  }
  for (const auto& entry : document.freshness.family_validity_nanos) {
    if (entry.first >= MetricFamily::Count) {
      return Status::error(StatusCode::Invalid, "freshness override family is out of range");
    }
    if (entry.second <= 0) {
      return Status::error(StatusCode::Invalid,
                           "freshness override must be positive for family " +
                               std::string(to_string(entry.first)));
    }
  }
  if (!document.aggregation.severity.is_total_order()) {
    return Status::error(StatusCode::Invalid,
                         "severity ranks must be a total order over every quality state");
  }
  return Status::success();
}

std::string canonical_policy_text(const PolicyDocument& document) {
  std::string out;
  out.append("policy ");
  out.append(document.id.value());
  out.append("\nrequire-capability ");
  out.append(document.require_capability ? "true" : "false");
  out.append("\nfreshness default-validity-ns ");
  out.append(text::format_i64(document.freshness.default_validity_nanos));
  out.append(" continuity-gap-ns ");
  out.append(text::format_i64(document.freshness.default_continuity_gap_nanos));
  out.append(" min-rate-interval-ns ");
  out.append(text::format_i64(document.freshness.min_rate_interval_nanos));
  out.push_back('\n');
  std::vector<std::pair<MetricFamily, i64>> overrides = document.freshness.family_validity_nanos;
  std::sort(overrides.begin(), overrides.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (const auto& entry : overrides) {
    out.append("freshness-family ");
    out.append(to_string(entry.first));
    out.push_back(' ');
    out.append(text::format_i64(entry.second));
    out.push_back('\n');
  }
  for (std::size_t index = 0; index < kQualityStateCount; ++index) {
    out.append("severity ");
    out.append(to_string(static_cast<QualityState>(index)));
    out.push_back(' ');
    out.append(text::format_u64(document.aggregation.severity.rank[index]));
    out.push_back('\n');
  }
  out.append("conflicting-dominates ");
  out.append(document.aggregation.conflicting_dominates ? "true" : "false");
  out.push_back('\n');

  std::vector<MetricRule> rules = document.rules;
  std::sort(rules.begin(), rules.end(),
            [](const MetricRule& lhs, const MetricRule& rhs) { return lhs.metric < rhs.metric; });
  for (const MetricRule& rule : rules) {
    out.append("rule ");
    out.append(render_metric_id(rule.metric));
    out.push_back(' ');
    out.append(to_string(rule.basis));
    out.append(" tolerance ");
    append_double(out, rule.conflict_tolerance);
    out.append(" validity-ns ");
    out.append(text::format_i64(rule.validity_nanos));
    out.append(" gap-ns ");
    out.append(text::format_i64(rule.max_continuity_gap_nanos));
    out.append(" wrap-inference ");
    out.append(rule.wrap_inference ? "true" : "false");
    out.append(" wrap-ceiling ");
    append_double(out, rule.wrap_ceiling_fraction);
    out.append(" wrap-floor ");
    append_double(out, rule.wrap_floor_fraction);
    out.push_back('\n');
    for (const ThresholdBand& band : rule.bands) {
      out.append("  band ");
      out.append(band.id.value());
      out.push_back(' ');
      append_double(out, band.lower);
      out.push_back(' ');
      if (band.upper.has_value()) {
        append_double(out, *band.upper);
      } else {
        out.append("+inf-bound");
      }
      out.push_back(' ');
      out.append(to_string(band.state));
      out.push_back('\n');
    }
  }
  std::vector<MetricId> unsupported = document.unsupported_metrics;
  std::sort(unsupported.begin(), unsupported.end());
  for (const MetricId& metric : unsupported) {
    out.append("unsupported ");
    out.append(render_metric_id(metric));
    out.push_back('\n');
  }
  return out;
}

CounterContinuityConfig counter_config_for(const PolicyDocument& document,
                                           const MetricId& metric) {
  CounterContinuityConfig config;
  const MetricRule* rule = document.find_rule(metric);
  if (rule != nullptr && rule->max_continuity_gap_nanos > 0) {
    config.max_gap_nanos = rule->max_continuity_gap_nanos;
  } else {
    config.max_gap_nanos = document.freshness.default_continuity_gap_nanos;
  }
  if (rule != nullptr) {
    config.wrap_inference = rule->wrap_inference;
    config.wrap_ceiling_fraction = rule->wrap_ceiling_fraction;
    config.wrap_floor_fraction = rule->wrap_floor_fraction;
  }
  return config;
}

PolicyDocument default_policy_document() {
  PolicyDocument document;
  document.id = PolicyId(std::string("default"));
  document.require_capability = true;

  const auto rule_for = [](MetricFamily family, const char* name, MetricBasis basis,
                           double tolerance, std::vector<ThresholdBand> bands) {
    MetricRule rule;
    rule.metric = MetricId(family, name);
    rule.basis = basis;
    rule.conflict_tolerance = tolerance;
    rule.bands = std::move(bands);
    return rule;
  };

  const auto band = [](const char* id, double lower, std::optional<double> upper,
                       QualityState state) {
    ThresholdBand entry;
    entry.id = RuleId(std::string(id));
    entry.lower = lower;
    entry.upper = upper;
    entry.state = state;
    return entry;
  };

  document.rules.push_back(rule_for(
      MetricFamily::SignalPower, "rx.level", MetricBasis::GaugeValue, 0.5,
      {band("rx-level-severe", kNegativeInfinity, -14.0, QualityState::Severe),
       band("rx-level-degraded", -14.0, -11.0, QualityState::Degraded),
       band("rx-level-marginal", -11.0, -7.0, QualityState::Marginal),
       band("rx-level-healthy", -7.0, std::nullopt, QualityState::Healthy)}));

  document.rules.push_back(rule_for(
      MetricFamily::SignalRatio, "snr", MetricBasis::GaugeValue, 0.5,
      {band("snr-severe", kNegativeInfinity, 12.0, QualityState::Severe),
       band("snr-degraded", 12.0, 16.0, QualityState::Degraded),
       band("snr-marginal", 16.0, 22.0, QualityState::Marginal),
       band("snr-healthy", 22.0, std::nullopt, QualityState::Healthy)}));

  document.rules.push_back(rule_for(
      MetricFamily::SignalRatio, "margin", MetricBasis::GaugeValue, 0.25,
      {band("margin-severe", kNegativeInfinity, 0.0, QualityState::Severe),
       band("margin-degraded", 0.0, 2.0, QualityState::Degraded),
       band("margin-marginal", 2.0, 4.0, QualityState::Marginal),
       band("margin-healthy", 4.0, std::nullopt, QualityState::Healthy)}));

  document.rules.push_back(rule_for(
      MetricFamily::ErrorCounter, "errors.uncorrectable", MetricBasis::CounterRatePerSecond, 0.0,
      {band("uncorrectable-healthy", kNegativeInfinity, 0.001, QualityState::Healthy),
       band("uncorrectable-marginal", 0.001, 0.1, QualityState::Marginal),
       band("uncorrectable-degraded", 0.1, 10.0, QualityState::Degraded),
       band("uncorrectable-severe", 10.0, std::nullopt, QualityState::Severe)}));

  document.rules.push_back(rule_for(
      MetricFamily::ErrorCounter, "errors.corrected", MetricBasis::CounterRatePerSecond, 0.0,
      {band("corrected-healthy", kNegativeInfinity, 1.0, QualityState::Healthy),
       band("corrected-marginal", 1.0, 100.0, QualityState::Marginal),
       band("corrected-degraded", 100.0, 10000.0, QualityState::Degraded),
       band("corrected-severe", 10000.0, std::nullopt, QualityState::Severe)}));

  document.rules.push_back(rule_for(
      MetricFamily::ErrorCounter, "errors.frame", MetricBasis::CounterRatePerSecond, 0.0,
      {band("frame-errors-healthy", kNegativeInfinity, 0.1, QualityState::Healthy),
       band("frame-errors-marginal", 0.1, 10.0, QualityState::Marginal),
       band("frame-errors-degraded", 10.0, 1000.0, QualityState::Degraded),
       band("frame-errors-severe", 1000.0, std::nullopt, QualityState::Severe)}));

  document.rules.push_back(rule_for(
      MetricFamily::ErrorCounter, "errors.symbol", MetricBasis::CounterRatePerSecond, 0.0,
      {band("symbol-errors-healthy", kNegativeInfinity, 1.0, QualityState::Healthy),
       band("symbol-errors-marginal", 1.0, 100.0, QualityState::Marginal),
       band("symbol-errors-degraded", 100.0, 100000.0, QualityState::Degraded),
       band("symbol-errors-severe", 100000.0, std::nullopt, QualityState::Severe)}));

  document.rules.push_back(rule_for(
      MetricFamily::LossRatio, "loss.ratio", MetricBasis::GaugeValue, 0.5,
      {band("loss-healthy", kNegativeInfinity, 1.0, QualityState::Healthy),
       band("loss-marginal", 1.0, 100.0, QualityState::Marginal),
       band("loss-degraded", 100.0, 1000.0, QualityState::Degraded),
       band("loss-severe", 1000.0, std::nullopt, QualityState::Severe)}));

  document.freshness.default_validity_nanos = 15'000'000'000LL;
  document.freshness.default_continuity_gap_nanos = 60'000'000'000LL;
  document.freshness.min_rate_interval_nanos = 1'000'000LL;
  document.freshness.family_validity_nanos = {
      {MetricFamily::Environmental, 60'000'000'000LL},
      {MetricFamily::Bias, 60'000'000'000LL},
      {MetricFamily::Timing, 5'000'000'000LL},
  };
  document.aggregation.severity = SeverityTable::defaults();
  document.aggregation.conflicting_dominates = true;
  return document;
}

PolicyRegistry::PolicyRegistry(PolicyDocument initial, Timestamp published_at,
                               std::size_t max_generations)
    : max_generations_(max_generations == 0 ? 1 : max_generations) {
  PolicyGenerationRecord record;
  record.stamp.id = initial.id;
  record.stamp.generation = PolicyGeneration(1);
  record.stamp.content_hash = Sha256::hex(Sha256::digest(canonical_policy_text(initial)));
  record.published_at = published_at;
  record.document = std::move(initial);
  generations_.push_back(std::move(record));
}

Status PolicyRegistry::validate_and_install(const PolicyDocument& document, Timestamp published_at,
                                            const MetricCatalog& catalog, std::size_t max_rules,
                                            std::size_t max_bands_per_rule,
                                            PolicyGenerationRecord& out) {
  const Status status = validate_policy(document, catalog, max_rules, max_bands_per_rule);
  if (!status.ok()) {
    return status;
  }
  locks::UniqueLock guard(mutex_);
  PolicyGenerationRecord record;
  record.stamp.id = document.id;
  record.stamp.generation = generations_.back().stamp.generation.next();
  const std::string canonical = canonical_policy_text(document);
  record.stamp.content_hash = Sha256::hex(Sha256::digest(canonical));
  record.published_at = published_at;
  record.document = document;
  generations_.push_back(record);
  while (generations_.size() > max_generations_) {
    generations_.erase(generations_.begin());
  }
  out = std::move(record);
  return Status::success();
}

Outcome<PolicyGenerationRecord> PolicyRegistry::publish(const PolicyDocument& document,
                                                       Timestamp published_at,
                                                       const MetricCatalog& catalog,
                                                       std::size_t max_rules,
                                                       std::size_t max_bands_per_rule) {
  PolicyGenerationRecord record;
  const Status status = validate_and_install(document, published_at, catalog, max_rules,
                                             max_bands_per_rule, record);
  if (!status.ok()) {
    return status;
  }
  return record;
}

Status PolicyRegistry::restore(const PolicyGenerationRecord& record, const MetricCatalog& catalog,
                               std::size_t max_rules, std::size_t max_bands_per_rule) {
  const Status validation =
      validate_policy(record.document, catalog, max_rules, max_bands_per_rule);
  if (!validation.ok()) {
    return validation;
  }
  const std::string hash = Sha256::hex(Sha256::digest(canonical_policy_text(record.document)));
  locks::UniqueLock guard(mutex_);
  for (const PolicyGenerationRecord& existing : generations_) {
    if (existing.stamp.generation == record.stamp.generation) {
      if (existing.stamp.content_hash == hash) {
        return Status::error(StatusCode::Duplicate, "policy generation already restored");
      }
      return Status::error(StatusCode::IdMismatch,
                           "policy generation number was re-used with different content");
    }
  }
  if (record.stamp.generation != generations_.back().stamp.generation.next()) {
    return Status::error(StatusCode::Corrupt,
                         "recovered policy generation is not the next generation");
  }
  PolicyGenerationRecord stored = record;
  stored.stamp.content_hash = hash;
  generations_.push_back(std::move(stored));
  while (generations_.size() > max_generations_) {
    generations_.erase(generations_.begin());
  }
  return Status::success();
}

PolicyGenerationRecord PolicyRegistry::current() const {
  locks::SharedLock guard(mutex_);
  return generations_.back();
}

std::optional<PolicyGenerationRecord> PolicyRegistry::get(PolicyGeneration generation) const {
  locks::SharedLock guard(mutex_);
  for (const PolicyGenerationRecord& record : generations_) {
    if (record.stamp.generation == generation) {
      return record;
    }
  }
  return std::nullopt;
}

std::vector<PolicyStamp> PolicyRegistry::history() const {
  locks::SharedLock guard(mutex_);
  std::vector<PolicyStamp> stamps;
  stamps.reserve(generations_.size());
  for (const PolicyGenerationRecord& record : generations_) {
    stamps.push_back(record.stamp);
  }
  return stamps;
}

std::size_t PolicyRegistry::generation_count() const {
  locks::SharedLock guard(mutex_);
  return generations_.size();
}

}  // namespace lqf
