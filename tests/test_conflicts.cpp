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

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");

struct TwoSources {
  std::shared_ptr<Fabric> fabric{};
  std::shared_ptr<ManualClock> clock{};
  LinkIdentity link{};
  SourceIdentity first{};
  SourceIdentity second{};
};

Outcome<TwoSources> make_two_sources() {
  auto fixture = Fixture::create();
  if (!fixture.ok()) {
    return fixture.status();
  }
  TwoSources sources;
  sources.fabric = fixture.value()->fabric;
  sources.clock = fixture.value()->clock;
  sources.link = fixture.value()->link;
  sources.first = fixture.value()->source;
  sources.second = SourceIdentity(SourceId(std::string("src-b")), SourceIncarnation(1));
  const Status first_declared = sources.fabric->declare_capability(
      make_capability(sources.link, sources.first, {kRxLevel})).status();
  if (!first_declared.ok()) {
    return first_declared;
  }
  const Status second_declared = sources.fabric->declare_capability(
      make_capability(sources.link, sources.second, {kRxLevel})).status();
  if (!second_declared.ok()) {
    return second_declared;
  }
  return sources;
}

}  // namespace

LQF_TEST(conflicts, equal_authority_disagreement_stays_conflicting) {
  Outcome<TwoSources> sources = make_two_sources();
  LQF_CHECK_STATUS_OK(sources.status());
  const TwoSources& context = sources.value();

  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.first, kRxLevel, -3.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));
  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.second, kRxLevel, -13.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));

  QualityQuery query;
  query.link = context.link;
  Outcome<LinkQualityReport> report = context.fabric->query(query);
  LQF_CHECK(report.ok());
  const MetricAssessment& assessment = report.value().metrics[0];
  LQF_CHECK(assessment.state == QualityState::Conflicting);
  LQF_CHECK(report.value().overall == QualityState::Conflicting);
  LQF_CHECK(has_reason(assessment.reasons, ReasonCode::EqualAuthorityDisagreement));
  // Both provenances survive, and no representative value is invented.
  LQF_CHECK(!assessment.value.has_value());
  LQF_CHECK(!assessment.rate_per_second.has_value());
  LQF_CHECK(assessment.disagreement.has_value());
  LQF_CHECK_EQ(assessment.disagreement.value(), 10.0);
  LQF_CHECK_EQ(assessment.sources_conflicting, std::uint32_t{2});

  bool saw_first = false;
  bool saw_second = false;
  for (const EvidenceRef& reference : assessment.evidence) {
    if (reference.source == context.first) {
      saw_first = true;
    }
    if (reference.source == context.second) {
      saw_second = true;
    }
  }
  LQF_CHECK(saw_first);
  LQF_CHECK(saw_second);

  // The inspection surface reports the same conflict with both claimants.
  InspectionReport inspection = context.fabric->inspect(InspectionFilter{}).value();
  LQF_CHECK_EQ(inspection.conflicts.size(), std::size_t{1});
  LQF_CHECK_EQ(inspection.conflicts[0].claimants.size(), std::size_t{2});
  LQF_CHECK(inspection.conflicts[0].spread == 10.0);
}

LQF_TEST(conflicts, agreement_inside_tolerance_is_not_a_conflict) {
  Outcome<TwoSources> sources = make_two_sources();
  LQF_CHECK_STATUS_OK(sources.status());
  const TwoSources& context = sources.value();

  PolicyDocument document = default_policy_document();
  for (MetricRule& rule : document.rules) {
    if (rule.metric == kRxLevel) {
      rule.conflict_tolerance = 2.0;
    }
  }
  LQF_CHECK_STATUS_OK(context.fabric->publish_policy(document));

  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.first, kRxLevel, -3.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));
  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.second, kRxLevel, -4.5, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));

  QualityQuery query;
  query.link = context.link;
  Outcome<LinkQualityReport> report = context.fabric->query(query);
  LQF_CHECK(report.value().metrics[0].state == QualityState::Healthy);
  LQF_CHECK(report.value().metrics[0].value.has_value());
  LQF_CHECK(report.value().metrics[0].confidence == ConfidenceLevel::High);

  // Just outside the tolerance it conflicts again, deterministically.
  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.second, kRxLevel, -6.0, 2, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));
  Outcome<LinkQualityReport> conflicting = context.fabric->query(query);
  LQF_CHECK(conflicting.value().metrics[0].state == QualityState::Conflicting);
}

LQF_TEST(conflicts, higher_authority_wins_and_is_recorded) {
  Outcome<TwoSources> sources = make_two_sources();
  LQF_CHECK_STATUS_OK(sources.status());
  const TwoSources& context = sources.value();

  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.first, kRxLevel, -3.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt, std::nullopt, AuthorityRank(1))));
  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.second, kRxLevel, -13.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt, std::nullopt, AuthorityRank(5))));

  QualityQuery query;
  query.link = context.link;
  Outcome<LinkQualityReport> report = context.fabric->query(query);
  LQF_CHECK(report.value().metrics[0].state == QualityState::Degraded);
  LQF_CHECK(report.value().metrics[0].value.has_value());
  LQF_CHECK_EQ(report.value().metrics[0].value.value(), -13.0);
  LQF_CHECK(has_reason(report.value().metrics[0].reasons, ReasonCode::LowerAuthorityIgnored));
}

LQF_TEST(conflicts, stale_source_does_not_conflict_with_a_fresh_one) {
  Outcome<TwoSources> sources = make_two_sources();
  LQF_CHECK_STATUS_OK(sources.status());
  const TwoSources& context = sources.value();

  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.first, kRxLevel, -13.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));
  context.clock->advance_seconds(30);
  LQF_CHECK_STATUS_OK(context.fabric->ingest(
      make_gauge(context.link, context.second, kRxLevel, -3.0, 1, context.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt)));

  QualityQuery query;
  query.link = context.link;
  Outcome<LinkQualityReport> report = context.fabric->query(query);
  // Only fresh evidence participates, so the stale source is not a conflict.
  LQF_CHECK(report.value().metrics[0].state == QualityState::Healthy);
  LQF_CHECK_EQ(report.value().metrics[0].sources_fresh, std::uint32_t{1});
  LQF_CHECK(has_reason(report.value().metrics[0].reasons, ReasonCode::PartiallyFreshEvidence) ||
            report.value().metrics[0].evidence.size() >= 1);
}
