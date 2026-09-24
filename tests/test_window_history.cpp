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
const MetricId kSnr(MetricFamily::SignalRatio, "snr");

}  // namespace

LQF_TEST(window_history, window_filters_and_orders_deterministically) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(self.fabric->declare_capability(
      make_capability(self.link, self.source, {kRxLevel, kSnr})));

  const i64 base = self.clock->now().wall_nanos;
  for (u64 index = 0; index < 5; ++index) {
    LQF_CHECK_STATUS_OK(self.fabric->ingest(
        make_gauge(self.link, self.source, kRxLevel, -3.0 - static_cast<double>(index), index + 1,
                   base + static_cast<i64>(index) * 1'000'000'000LL, Unit::DecibelMilliwatt)));
    LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kSnr,
                                                       30.0 + static_cast<double>(index), index + 1,
                                                       base + static_cast<i64>(index) * 1'000'000'000LL,
                                                       Unit::Decibel)));
  }

  WindowQuery all;
  all.link = self.link;
  all.limit = 64;
  Outcome<WindowResult> everything = self.fabric->window(all);
  LQF_CHECK(everything.ok());
  LQF_CHECK_EQ(everything.value().records.size(), std::size_t{10});
  LQF_CHECK_EQ(everything.value().matched, u64{10});
  LQF_CHECK(!everything.value().truncated);
  LQF_CHECK(everything.value().has_available_range);
  for (std::size_t index = 1; index < everything.value().records.size(); ++index) {
    LQF_CHECK(everything.value().records[index - 1].observation.observed_at.unix_nanos <=
              everything.value().records[index].observation.observed_at.unix_nanos);
  }

  WindowQuery filtered;
  filtered.link = self.link;
  filtered.metric = kRxLevel;
  filtered.limit = 64;
  Outcome<WindowResult> only_rx = self.fabric->window(filtered);
  LQF_CHECK_EQ(only_rx.value().records.size(), std::size_t{5});
  for (const EvidenceRecord& record : only_rx.value().records) {
    LQF_CHECK(record.observation.metric == kRxLevel);
  }

  WindowQuery ranged;
  ranged.link = self.link;
  ranged.axis = WindowAxis::ObservationTime;
  ranged.from_nanos = base + 1'000'000'000LL;
  ranged.to_nanos = base + 3'000'000'000LL;
  ranged.limit = 64;
  Outcome<WindowResult> range = self.fabric->window(ranged);
  LQF_CHECK_EQ(range.value().records.size(), std::size_t{4});
  LQF_CHECK_EQ(range.value().matched, u64{4});

  WindowQuery limited;
  limited.link = self.link;
  limited.limit = 3;
  Outcome<WindowResult> truncated = self.fabric->window(limited);
  LQF_CHECK_EQ(truncated.value().records.size(), std::size_t{3});
  LQF_CHECK(truncated.value().truncated);
  LQF_CHECK_EQ(truncated.value().matched, u64{10});

  // An inverted range is refused rather than silently empty.
  WindowQuery inverted = ranged;
  inverted.from_nanos = ranged.to_nanos;
  inverted.to_nanos = ranged.from_nanos;
  LQF_CHECK(!self.fabric->window(inverted).ok());
}

LQF_TEST(window_history, window_by_receive_time_and_source_filter) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  const SourceIdentity second(SourceId(std::string("src-b")), SourceIncarnation(1));
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, second, {kRxLevel})));

  const i64 base = self.clock->now().wall_nanos;
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     base, Unit::DecibelMilliwatt)));
  self.clock->advance_seconds(10);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, second, kRxLevel, -4.0, 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));

  WindowQuery by_receive;
  by_receive.link = self.link;
  by_receive.axis = WindowAxis::ReceiveTime;
  by_receive.from_nanos = base + 5'000'000'000LL;
  by_receive.to_nanos = base + 20'000'000'000LL;
  by_receive.limit = 64;
  Outcome<WindowResult> recent = self.fabric->window(by_receive);
  LQF_CHECK_EQ(recent.value().records.size(), std::size_t{1});
  LQF_CHECK(recent.value().records[0].observation.source == second);

  WindowQuery by_source;
  by_source.link = self.link;
  by_source.source = self.source.id;
  by_source.limit = 64;
  Outcome<WindowResult> first_only = self.fabric->window(by_source);
  LQF_CHECK_EQ(first_only.value().records.size(), std::size_t{1});
  LQF_CHECK(first_only.value().records[0].observation.source == self.source);
}

LQF_TEST(window_history, as_of_assessment_uses_the_evidence_of_that_instant) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));

  const i64 base = self.clock->now().wall_nanos;
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     base, Unit::DecibelMilliwatt)));
  self.clock->advance_seconds(20);
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -13.0, 2,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));

  const i64 as_of = self.clock->now().steady_nanos - 10'000'000'000LL;
  WindowQuery query;
  query.link = self.link;
  query.limit = 64;
  query.assess_as_of = true;
  query.as_of_steady_nanos = as_of;
  Outcome<WindowResult> result = self.fabric->window(query);
  LQF_CHECK(result.ok());
  LQF_CHECK(result.value().as_of_report.has_value());
  // Ten seconds ago the healthy reading was only ten seconds old, so the
  // assessment at that instant is healthy; the newer degraded reading did not
  // exist yet.
  LQF_CHECK(result.value().as_of_report->overall == QualityState::Healthy);

  // The current state is degraded: history and present are different questions.
  QualityQuery current;
  current.link = self.link;
  LQF_CHECK(self.fabric->query(current).value().overall == QualityState::Degraded);

  // An as-of instant in the future is refused.
  WindowQuery future = query;
  future.as_of_steady_nanos = self.clock->now().steady_nanos + 1'000'000'000LL;
  LQF_CHECK(!self.fabric->window(future).ok());
}

LQF_TEST(window_history, retained_history_is_bounded) {
  FabricConfig config;
  config.limits.max_global_history = 8;
  config.limits.max_stream_history = 4;
  config.forced_epoch = FabricEpoch(7);
  config.clock = std::make_shared<ManualClock>();
  auto clock = std::static_pointer_cast<ManualClock>(config.clock);
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());
  const LinkIdentity link(LinkId(std::string("link-bounded")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src")), SourceIncarnation(1));
  LQF_CHECK_STATUS_OK(
      fabric.value()->declare_capability(make_capability(link, source, {kRxLevel})));
  for (u64 index = 0; index < 20; ++index) {
    LQF_CHECK_STATUS_OK(fabric.value()->ingest(
        make_gauge(link, source, kRxLevel, -3.0, index + 1, clock->now().wall_nanos,
                   Unit::DecibelMilliwatt)));
    clock->advance_seconds(1);
  }
  const FabricStats stats = fabric.value()->stats();
  LQF_CHECK(stats.retained_records <= 8);
  WindowQuery query;
  query.link = link;
  query.limit = 64;
  Outcome<WindowResult> result = fabric.value()->window(query);
  LQF_CHECK(result.value().records.size() <= 8);
  LQF_CHECK(result.value().truncated || result.value().records.size() == 8);
}
