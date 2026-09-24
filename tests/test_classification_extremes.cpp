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

#include <cmath>
#include <limits>

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const MetricId kMargin(MetricFamily::SignalRatio, "margin");

PolicyDocument boundary_policy() {
  PolicyDocument document = default_policy_document();
  for (MetricRule& rule : document.rules) {
    if (rule.metric == kMargin) {
      rule.bands.clear();
      ThresholdBand severe;
      severe.id = RuleId(std::string("margin-severe"));
      severe.lower = -std::numeric_limits<double>::infinity();
      severe.upper = 0.0;
      severe.state = QualityState::Severe;
      ThresholdBand marginal;
      marginal.id = RuleId(std::string("margin-marginal"));
      marginal.lower = 0.0;
      marginal.upper = 10.0;
      marginal.state = QualityState::Marginal;
      ThresholdBand healthy;
      healthy.id = RuleId(std::string("margin-healthy"));
      healthy.lower = 10.0;
      healthy.state = QualityState::Healthy;
      rule.bands.push_back(severe);
      rule.bands.push_back(marginal);
      rule.bands.push_back(healthy);
    }
  }
  return document;
}

}  // namespace

LQF_TEST(classification_extremes, band_edges_are_half_open_and_deterministic) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kMargin})));
  LQF_CHECK_STATUS_OK(self.fabric->publish_policy(boundary_policy()));

  QualityQuery query;
  query.link = self.link;
  const auto state_for = [&](double value, u64 sequence) {
    LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kMargin, value,
                                                       sequence, self.clock->now().wall_nanos,
                                                       Unit::Decibel)));
    Outcome<LinkQualityReport> report = self.fabric->query(query);
    LQF_CHECK(report.ok());
    return report.value().metrics[0].state;
  };

  // The lower bound is inclusive and the upper bound is exclusive.
  LQF_CHECK(state_for(0.0, 1) == QualityState::Marginal);
  LQF_CHECK(state_for(9.999999, 2) == QualityState::Marginal);
  LQF_CHECK(state_for(10.0, 3) == QualityState::Healthy);
  LQF_CHECK(state_for(-0.000001, 4) == QualityState::Severe);
  LQF_CHECK(state_for(-1.0e300, 5) == QualityState::Severe);
  LQF_CHECK(state_for(1.0e300, 6) == QualityState::Healthy);
  // The smallest positive denormal still classifies.
  LQF_CHECK(state_for(std::numeric_limits<double>::denorm_min(), 7) == QualityState::Marginal);
}

LQF_TEST(classification_extremes, non_finite_samples_are_refused_at_the_boundary) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));

  const double invalid[] = {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()};
  u64 sequence = 1;
  for (const double value : invalid) {
    const Outcome<IngestOutcome> refused = self.fabric->ingest(
        make_gauge(self.link, self.source, kRxLevel, value, sequence++,
                   self.clock->now().wall_nanos, Unit::DecibelMilliwatt));
    LQF_CHECK(!refused.ok());
    LQF_CHECK(refused.status().code() == StatusCode::Invalid);
  }
  // Nothing was stored, so the metric stays unknown rather than becoming zero.
  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.value().metrics[0].state == QualityState::Unknown);
  LQF_CHECK(!report.value().metrics[0].value.has_value());
}

LQF_TEST(classification_extremes, extreme_values_do_not_overflow_classification) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));

  // A doubly extreme value cannot produce an infinite disagreement or a NaN.
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel,
                                                     -std::numeric_limits<double>::max(), 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));
  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK(report.value().metrics[0].state == QualityState::Severe);
  LQF_CHECK(report.value().metrics[0].value.has_value());
  LQF_CHECK(std::isfinite(report.value().metrics[0].value.value()));
}

LQF_TEST(classification_extremes, extreme_counter_values_stay_finite) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  const MetricId errors(MetricFamily::ErrorCounter, "errors.uncorrectable");
  CapabilityDeclaration declaration = make_capability(self.link, self.source, {errors});
  declaration.metrics[0].lanes_declared = false;
  LQF_CHECK_STATUS_OK(self.fabric->declare_capability(declaration));

  const i64 start = self.clock->now().wall_nanos;
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_counter(self.link, self.source, errors, 0, 1, start,
                                                       Unit::Count)));
  self.clock->advance_seconds(1);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(
      make_counter(self.link, self.source, errors, std::numeric_limits<u64>::max(), 2,
                   self.clock->now().wall_nanos, Unit::Count)));

  QualityQuery query;
  query.link = self.link;
  Outcome<LinkQualityReport> report = self.fabric->query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK(report.value().metrics[0].rate_per_second.has_value());
  const double rate = report.value().metrics[0].rate_per_second.value();
  LQF_CHECK(std::isfinite(rate));
  LQF_CHECK(rate > 1.0e19);
  LQF_CHECK(report.value().metrics[0].state == QualityState::Severe);
}
