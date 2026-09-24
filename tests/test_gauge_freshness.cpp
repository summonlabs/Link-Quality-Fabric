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
const MetricId kErrors(MetricFamily::ErrorCounter, "errors.uncorrectable");

i64 now_observed(const std::shared_ptr<ManualClock>& clock) { return clock->now().wall_nanos; }

}  // namespace

LQF_TEST(gauge_freshness, fresh_gauge_classifies_and_expires) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));

  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     now_observed(self.clock),
                                                     Unit::DecibelMilliwatt)));

  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK_EQ(report.value().metrics.size(), std::size_t{1});
  LQF_CHECK(report.value().metrics[0].state == QualityState::Healthy);
  LQF_CHECK(report.value().overall == QualityState::Healthy);
  LQF_CHECK_EQ(report.value().evidence_fresh, u64{1});
  LQF_CHECK(has_reason(report.value().metrics[0].reasons, ReasonCode::BandMatched));

  // Beyond the declared validity window the same evidence is stale, and stale
  // evidence never asserts current health.
  self.clock->advance_seconds(16);
  Outcome<LinkQualityReport> stale = self.fabric->query(query);
  LQF_CHECK(stale.ok());
  LQF_CHECK(stale.value().metrics[0].state == QualityState::Stale);
  LQF_CHECK(stale.value().overall == QualityState::Stale);
  LQF_CHECK(!stale.value().metrics[0].value.has_value());
  LQF_CHECK(has_reason(stale.value().metrics[0].reasons, ReasonCode::EvidenceExpired));
  LQF_CHECK_EQ(stale.value().evidence_fresh, u64{0});

  // Fresh evidence restores the classification without rewriting history.
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -13.0, 2,
                                                     now_observed(self.clock),
                                                     Unit::DecibelMilliwatt)));
  Outcome<LinkQualityReport> degraded = self.fabric->query(query);
  LQF_CHECK(degraded.value().metrics[0].state == QualityState::Degraded);
}

LQF_TEST(gauge_freshness, missing_evidence_is_never_zero_and_never_healthy) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();

  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> unknown = self.fabric->query(query);
  LQF_CHECK(unknown.ok());
  LQF_CHECK(unknown.value().overall == QualityState::Unsupported);
  LQF_CHECK(has_reason(unknown.value().reasons, ReasonCode::NoSourcesForLink));
  LQF_CHECK(has_reason(unknown.value().reasons, ReasonCode::LinkNotRegistered));

  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  Outcome<LinkQualityReport> no_evidence = self.fabric->query(query);
  LQF_CHECK_EQ(no_evidence.value().metrics.size(), std::size_t{1});
  LQF_CHECK(no_evidence.value().metrics[0].state == QualityState::Unknown);
  LQF_CHECK(has_reason(no_evidence.value().metrics[0].reasons, ReasonCode::NoEvidence));
  LQF_CHECK(!no_evidence.value().metrics[0].value.has_value());
  LQF_CHECK(no_evidence.value().metrics[0].confidence == ConfidenceLevel::None);
}

LQF_TEST(gauge_freshness, undeclared_metric_stays_unsupported) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  CapabilityDeclaration declaration = make_capability(self.link, self.source, {kRxLevel});
  declaration.unsupported_metrics.push_back(kErrors);
  LQF_CHECK_STATUS_OK(self.fabric->declare_capability(declaration));

  QualityQuery query;
  query.link = self.link;
  query.metrics.push_back(kErrors);
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK_EQ(report.value().metrics.size(), std::size_t{1});
  LQF_CHECK(report.value().metrics[0].state == QualityState::Unsupported);
  LQF_CHECK(report.value().metrics[0].confidence == ConfidenceLevel::None);
  LQF_CHECK(has_reason(report.value().metrics[0].reasons, ReasonCode::DeclaredUnsupported));
  LQF_CHECK(!report.value().metrics[0].value.has_value());
}

LQF_TEST(gauge_freshness, counter_rate_requires_two_fresh_readings) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kErrors})));

  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, kErrors, 0, 1,
                                                       now_observed(self.clock), Unit::Count)));
  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> single = self.fabric->query(query);
  LQF_CHECK(single.value().metrics[0].state == QualityState::Incomplete);
  LQF_CHECK(has_reason(single.value().metrics[0].reasons, ReasonCode::CounterBaselineOnly));
  LQF_CHECK(!single.value().metrics[0].value.has_value());

  self.clock->advance_seconds(1);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, kErrors, 0, 2,
                                                       now_observed(self.clock), Unit::Count)));
  Outcome<LinkQualityReport> healthy = self.fabric->query(query);
  LQF_CHECK(healthy.value().metrics[0].state == QualityState::Healthy);
  LQF_CHECK(healthy.value().metrics[0].rate_per_second.has_value());
  LQF_CHECK_EQ(healthy.value().metrics[0].rate_per_second.value(), 0.0);

  // An interval longer than the validity window cannot assert a current rate.
  self.clock->advance_seconds(40);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, kErrors, 5, 3,
                                                       now_observed(self.clock), Unit::Count)));
  Outcome<LinkQualityReport> long_interval = self.fabric->query(query);
  LQF_CHECK(long_interval.value().metrics[0].state == QualityState::Incomplete);
  LQF_CHECK(long_interval.value().metrics[0].interval_rate_per_second.has_value());
}

LQF_TEST(gauge_freshness, counter_decrease_is_incomplete_not_zero) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kErrors})));
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, kErrors, 5000, 1,
                                                       now_observed(self.clock), Unit::Count)));
  self.clock->advance_seconds(1);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, kErrors, 3, 2,
                                                       now_observed(self.clock), Unit::Count)));
  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.value().metrics[0].state == QualityState::Incomplete);
  LQF_CHECK(has_reason(report.value().metrics[0].reasons, ReasonCode::CounterDecreaseAmbiguous));
  LQF_CHECK(!report.value().metrics[0].value.has_value());
  LQF_CHECK(!report.value().metrics[0].rate_per_second.has_value());
  LQF_CHECK(report.value().overall != QualityState::Healthy);
}

LQF_TEST(gauge_freshness, required_capability_gates_ingestion) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  const Outcome<IngestOutcome> refused = self.fabric->ingest(
      make_gauge(self.link, self.source, kRxLevel, -3.0, 1, now_observed(self.clock),
                 Unit::DecibelMilliwatt));
  LQF_CHECK(!refused.ok());
  LQF_CHECK(refused.status().code() == StatusCode::Rejected);

  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  const Outcome<IngestOutcome> wrong_unit = self.fabric->ingest(
      make_gauge(self.link, self.source, kRxLevel, -3.0, 1, now_observed(self.clock), Unit::Decibel));
  LQF_CHECK(!wrong_unit.ok());
  LQF_CHECK(wrong_unit.status().code() == StatusCode::Invalid);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     now_observed(self.clock),
                                                     Unit::DecibelMilliwatt)));
}

LQF_TEST(gauge_freshness, unsynchronized_old_evidence_is_accepted) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  const i64 long_ago = now_observed(self.clock) - 3600000000000LL;
  Observation observation =
      make_gauge(self.link, self.source, kRxLevel, -3.0, 1, long_ago, Unit::DecibelMilliwatt);
  observation.observed_at.synchronized = false;
  LQF_CHECK_STATUS_OK(self.fabric->ingest(observation));

  Observation synchronized = observation;
  synchronized.observed_at.synchronized = true;
  synchronized.sequence = SequenceNumber(2);
  const Outcome<IngestOutcome> refused = self.fabric->ingest(synchronized);
  LQF_CHECK(!refused.ok());
  LQF_CHECK(refused.status().code() == StatusCode::Stale);
}
