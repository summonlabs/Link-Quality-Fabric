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

#include "harness.hpp"

#include <limits>

using namespace lqf;
using namespace lqf::test;

namespace {

constexpr double kNegativeInfinity = -std::numeric_limits<double>::infinity();

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");

PolicyDocument healthy_at_minus_twenty() {
  PolicyDocument document = default_policy_document();
  for (MetricRule& rule : document.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands.clear();
      ThresholdBand severe;
      severe.id = RuleId(std::string("rx-severe"));
      severe.lower = kNegativeInfinity;
      severe.upper = -30.0;
      severe.state = QualityState::Severe;
      ThresholdBand healthy;
      healthy.id = RuleId(std::string("rx-healthy"));
      healthy.lower = -30.0;
      healthy.state = QualityState::Healthy;
      rule.bands.push_back(severe);
      rule.bands.push_back(healthy);
    }
  }
  return document;
}

}  // namespace

LQF_TEST(policy_generation, validation_rejects_ambiguous_documents) {
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();

  PolicyDocument good = default_policy_document();
  LQF_CHECK_STATUS_OK(validate_policy(good, catalog, 128, 16));

  // A gap would leave a value unclassified, so it is refused.
  PolicyDocument gapped = good;
  for (MetricRule& rule : gapped.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands[1].lower = -15.0;
    }
  }
  LQF_CHECK(validate_policy(gapped, catalog, 128, 16).code() == StatusCode::Invalid);

  PolicyDocument overlapping = good;
  for (MetricRule& rule : overlapping.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands[1].upper = -10.0;
    }
  }
  LQF_CHECK(!validate_policy(overlapping, catalog, 128, 16).ok());

  PolicyDocument not_total = good;
  for (MetricRule& rule : not_total.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands.back().upper = 100.0;
    }
  }
  LQF_CHECK(!validate_policy(not_total, catalog, 128, 16).ok());

  PolicyDocument not_from_negative_infinity = good;
  for (MetricRule& rule : not_from_negative_infinity.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands.front().lower = -100.0;
    }
  }
  LQF_CHECK(!validate_policy(not_from_negative_infinity, catalog, 128, 16).ok());

  PolicyDocument nan_bound = good;
  for (MetricRule& rule : nan_bound.rules) {
    if (rule.metric == kRxLevel) {
      rule.bands[1].lower = std::numeric_limits<double>::quiet_NaN();
    }
  }
  LQF_CHECK(!validate_policy(nan_bound, catalog, 128, 16).ok());

  PolicyDocument negative_tolerance = good;
  negative_tolerance.rules[0].conflict_tolerance = -1.0;
  LQF_CHECK(!validate_policy(negative_tolerance, catalog, 128, 16).ok());

  PolicyDocument unknown_metric = good;
  unknown_metric.rules[0].metric = MetricId(MetricFamily::SignalPower, "rx.not-registered");
  LQF_CHECK(validate_policy(unknown_metric, catalog, 128, 16).code() == StatusCode::Unsupported);

  PolicyDocument both = good;
  both.unsupported_metrics.push_back(kRxLevel);
  LQF_CHECK(validate_policy(both, catalog, 128, 16).code() == StatusCode::Conflict);

  PolicyDocument bad_wrap = good;
  bad_wrap.rules[0].wrap_inference = true;
  bad_wrap.rules[0].wrap_ceiling_fraction = 0.2;
  bad_wrap.rules[0].wrap_floor_fraction = 0.8;
  LQF_CHECK(!validate_policy(bad_wrap, catalog, 128, 16).ok());

  PolicyDocument bad_severity = good;
  bad_severity.aggregation.severity.rank[0] = 0;
  bad_severity.aggregation.severity.rank[1] = 0;
  LQF_CHECK(!validate_policy(bad_severity, catalog, 128, 16).ok());

  PolicyDocument duplicate_rule = good;
  duplicate_rule.rules.push_back(duplicate_rule.rules[0]);
  LQF_CHECK(!validate_policy(duplicate_rule, catalog, 128, 16).ok());

  LQF_CHECK(validate_policy(good, catalog, 0, 16).code() == StatusCode::LimitExceeded);
}

LQF_TEST(policy_generation, publication_is_numbered_hashed_and_immutable) {
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  PolicyRegistry registry(default_policy_document(), Timestamp{1, true});
  LQF_CHECK_EQ(registry.current().stamp.generation.value(), u64{1});
  const std::string first_hash = registry.current().stamp.content_hash;
  LQF_CHECK(!first_hash.empty());

  Outcome<PolicyGenerationRecord> published =
      registry.publish(healthy_at_minus_twenty(), Timestamp{2, true}, catalog, 128, 16);
  LQF_CHECK(published.ok());
  LQF_CHECK_EQ(published.value().stamp.generation.value(), u64{2});
  LQF_CHECK(registry.current().stamp.content_hash != first_hash);
  LQF_CHECK_EQ(registry.generation_count(), std::size_t{2});

  // The same document published twice produces the same content hash: the
  // classification cannot drift with the generation number.
  Outcome<PolicyGenerationRecord> again =
      registry.publish(healthy_at_minus_twenty(), Timestamp{3, true}, catalog, 128, 16);
  LQF_CHECK(again.ok());
  LQF_CHECK_EQ(again.value().stamp.content_hash, published.value().stamp.content_hash);
  LQF_CHECK_EQ(again.value().stamp.generation.value(), u64{3});

  LQF_CHECK(registry.get(PolicyGeneration(2)).has_value());
  LQF_CHECK(!registry.get(PolicyGeneration(99)).has_value());
  const std::vector<PolicyStamp> history = registry.history();
  LQF_CHECK_EQ(history.size(), std::size_t{3});
  LQF_CHECK_EQ(history[0].generation.value(), u64{1});

  // Canonical text is deterministic and is what the hash covers.
  LQF_CHECK_EQ(canonical_policy_text(healthy_at_minus_twenty()),
               canonical_policy_text(healthy_at_minus_twenty()));
  LQF_CHECK(canonical_policy_text(healthy_at_minus_twenty()) !=
            canonical_policy_text(default_policy_document()));
}

LQF_TEST(policy_generation, retention_bound_drops_the_oldest_generations) {
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  PolicyRegistry registry(default_policy_document(), Timestamp{1, true}, 3);
  for (int index = 0; index < 5; ++index) {
    LQF_CHECK(registry.publish(healthy_at_minus_twenty(), Timestamp{2, true}, catalog, 128, 16).ok());
  }
  LQF_CHECK_EQ(registry.generation_count(), std::size_t{3});
  LQF_CHECK(!registry.get(PolicyGeneration(1)).has_value());
  LQF_CHECK(registry.get(PolicyGeneration(6)).has_value());
}

LQF_TEST(policy_generation, threshold_change_reclassifies_without_rewriting_evidence) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -20.0, 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));
  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> before = self.fabric->query(query);
  // -20 dBm is below the default degraded band, so it is severe before the
  // operator publishes thresholds that make it healthy.
  LQF_CHECK(before.value().metrics[0].state == QualityState::Severe);
  LQF_CHECK_EQ(before.value().policy.generation.value(), u64{1});

  WindowQuery window;
  window.link = self.link;
  window.limit = 64;
  Outcome<WindowResult> evidence_before = self.fabric->window(window);
  LQF_CHECK(evidence_before.ok());
  LQF_CHECK_EQ(evidence_before.value().records.size(), std::size_t{1});
  const EvidenceRecord raw_before = evidence_before.value().records[0];

  Outcome<PolicyStamp> published = self.fabric->publish_policy(healthy_at_minus_twenty());
  LQF_CHECK(published.ok());
  LQF_CHECK_EQ(published.value().generation.value(), u64{2});

  Outcome<LinkQualityReport> after = self.fabric->query(query);
  LQF_CHECK(after.value().metrics[0].state == QualityState::Healthy);
  LQF_CHECK_EQ(after.value().policy.generation.value(), u64{2});
  LQF_CHECK(after.value().policy.content_hash != before.value().policy.content_hash);

  // The raw evidence is byte for byte what it was: a policy never rewrites it.
  Outcome<WindowResult> evidence_after = self.fabric->window(window);
  LQF_CHECK(evidence_after.ok());
  LQF_CHECK_EQ(evidence_after.value().records.size(), std::size_t{1});
  LQF_CHECK(evidence_after.value().records[0] == raw_before);
  LQF_CHECK_EQ(render_evidence_record(evidence_after.value().records[0]),
               render_evidence_record(raw_before));

  // Re-deriving with the same generation is deterministic.
  Outcome<LinkQualityReport> repeat = self.fabric->query(query);
  LQF_CHECK_EQ(render_report(repeat.value()), render_report(after.value()));
}

LQF_TEST(policy_generation, rejected_publication_leaves_the_generation_unchanged) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  PolicyDocument invalid = default_policy_document();
  invalid.rules[0].bands[1].lower = -1.0;
  const Outcome<PolicyStamp> rejected = self.fabric->publish_policy(invalid);
  LQF_CHECK(!rejected.ok());
  LQF_CHECK(rejected.status().code() == StatusCode::Invalid);
  LQF_CHECK_EQ(self.fabric->current_policy().generation.value(), u64{1});
  LQF_CHECK_EQ(self.fabric->policy_history().size(), std::size_t{1});
}
