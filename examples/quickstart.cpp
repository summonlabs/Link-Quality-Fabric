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

// Quickstart: an in-memory fabric, one declared capability, two gauge readings
// and one counter pair, then a quality report.
//
// The states printed here are the ones the library derived. The checks below
// only compare what was returned with what this example expects; nothing is
// asserted on the library's behalf.

#include <cmath>
#include <iostream>
#include <memory>

#include "lqf/core/clock.hpp"
#include "lqf/core/text.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/runtime/fabric.hpp"

namespace {

const lqf::MetricId kRxLevel{lqf::MetricFamily::SignalPower, "rx.level"};
const lqf::MetricId kCorrectedErrors{lqf::MetricFamily::ErrorCounter, "errors.corrected"};

const lqf::MetricAssessment* find_metric(const lqf::LinkQualityReport& report,
                                         const lqf::MetricId& metric) {
  for (const lqf::MetricAssessment& assessment : report.metrics) {
    if (assessment.metric == metric) {
      return &assessment;
    }
  }
  return nullptr;
}

lqf::Observation gauge_observation(const lqf::LinkIdentity& link, const lqf::SourceIdentity& source,
                                   lqf::u64 sequence, double value, lqf::i64 observed_nanos) {
  lqf::Observation observation;
  observation.link = link;
  observation.source = source;
  observation.sequence = lqf::SequenceNumber(sequence);
  observation.metric = kRxLevel;
  observation.lane = lqf::LaneDimension::aggregate_dimension();
  observation.unit = lqf::Unit::DecibelMilliwatt;
  lqf::GaugeReading reading;
  reading.value = value;
  observation.reading = reading;
  observation.observed_at = lqf::Timestamp{observed_nanos, true};
  observation.provenance.transport = lqf::TransportKind::Synthetic;
  observation.provenance.evidence_class = lqf::EvidenceClass::Synthetic;
  observation.provenance.origin = "quickstart/generator";
  observation.provenance.producer = "lqf-example-quickstart";
  return observation;
}

lqf::Observation counter_observation(const lqf::LinkIdentity& link,
                                     const lqf::SourceIdentity& source, lqf::u64 sequence,
                                     lqf::u64 value, lqf::i64 observed_nanos) {
  lqf::Observation observation;
  observation.link = link;
  observation.source = source;
  observation.sequence = lqf::SequenceNumber(sequence);
  observation.metric = kCorrectedErrors;
  observation.lane = lqf::LaneDimension::aggregate_dimension();
  observation.unit = lqf::Unit::Count;
  lqf::CounterReading reading;
  reading.value = value;
  reading.width = lqf::CounterWidth::Bits64;
  reading.reset_declared = false;
  observation.reading = reading;
  observation.observed_at = lqf::Timestamp{observed_nanos, true};
  observation.provenance.transport = lqf::TransportKind::Synthetic;
  observation.provenance.evidence_class = lqf::EvidenceClass::Synthetic;
  observation.provenance.origin = "quickstart/generator";
  observation.provenance.producer = "lqf-example-quickstart";
  return observation;
}

}  // namespace

int main() {
  // A manual clock keeps the example deterministic: the counter pair below is
  // two seconds apart and nothing depends on how fast this process runs.
  const auto clock = std::make_shared<lqf::ManualClock>();

  lqf::FabricConfig config;
  config.clock = clock;
  config.journal.enabled = false;

  lqf::Outcome<std::shared_ptr<lqf::Fabric>> opened = lqf::Fabric::open(config);
  if (!opened.ok()) {
    std::cerr << "quickstart: the fabric could not be opened: " << opened.status().to_text()
              << "\n";
    return 1;
  }
  const std::shared_ptr<lqf::Fabric>& fabric = opened.value();

  const lqf::LinkIdentity link{lqf::LinkId(std::string("quickstart-link")),
                               lqf::LinkGeneration(1)};
  const lqf::SourceIdentity source{lqf::SourceId(std::string("quickstart-source")),
                                   lqf::SourceIncarnation(1)};

  lqf::CapabilityDeclaration declaration;
  declaration.source = source;
  declaration.link_scope = link;
  declaration.revision = lqf::CapabilityRevision(1);
  declaration.transport = lqf::TransportKind::Synthetic;
  declaration.evidence_class = lqf::EvidenceClass::Synthetic;
  declaration.declared_at = lqf::Timestamp{clock->now().wall_nanos, true};

  lqf::MetricCapability level;
  level.metric = kRxLevel;
  level.unit = lqf::Unit::DecibelMilliwatt;
  level.semantics = lqf::SampleSemantics::Gauge;
  level.lanes_declared = false;
  level.lane_count = 0;
  declaration.metrics.push_back(level);

  lqf::MetricCapability counters;
  counters.metric = kCorrectedErrors;
  counters.unit = lqf::Unit::Count;
  counters.semantics = lqf::SampleSemantics::Counter;
  counters.counter_width = lqf::CounterWidth::Bits64;
  counters.lanes_declared = false;
  counters.lane_count = 0;
  declaration.metrics.push_back(counters);

  lqf::Outcome<lqf::CapabilityAck> declared = fabric->declare_capability(declaration);
  if (!declared.ok()) {
    std::cerr << "quickstart: the capability was refused: " << declared.status().to_text() << "\n";
    return 1;
  }

  const lqf::i64 start_nanos = clock->now().wall_nanos;
  const auto ingest = [&fabric](const lqf::Observation& observation) {
    const lqf::Outcome<lqf::IngestOutcome> outcome = fabric->ingest(observation);
    if (!outcome.ok()) {
      std::cerr << "quickstart: evidence was refused: " << outcome.status().to_text() << "\n";
      return false;
    }
    return outcome.value().accepted;
  };

  // Two gauges 3.5 dB apart: -6.0 dBm is healthy, -8.5 dBm is marginal.
  if (!ingest(gauge_observation(link, source, 1, -6.0, start_nanos))) {
    return 1;
  }
  if (!ingest(gauge_observation(link, source, 2, -8.5, start_nanos))) {
    return 1;
  }

  // One counter pair: 1000 -> 1001 over two seconds is 0.5 corrected errors per
  // second, which the default policy classifies as healthy.
  if (!ingest(counter_observation(link, source, 3, 1000, start_nanos))) {
    return 1;
  }
  clock->advance_seconds(2);
  const lqf::i64 second_nanos = clock->now().wall_nanos;
  if (!ingest(counter_observation(link, source, 4, 1001, second_nanos))) {
    return 1;
  }

  lqf::QualityQuery query;
  query.link = link;
  query.include_evidence = true;

  lqf::Outcome<lqf::LinkQualityReport> queried = fabric->query(query);
  if (!queried.ok()) {
    std::cerr << "quickstart: the query failed: " << queried.status().to_text() << "\n";
    return 1;
  }
  const lqf::LinkQualityReport& report = queried.value();
  std::cout << lqf::render_report(report);

  if (!report.link_registered) {
    std::cerr << "quickstart: the link was not registered by ingest\n";
    return 1;
  }
  if (!report.evidence_present) {
    std::cerr << "quickstart: no live evidence reached the report\n";
    return 1;
  }
  if (report.metrics.size() != 2U) {
    std::cerr << "quickstart: expected two metric assessments, received " << report.metrics.size()
              << "\n";
    return 1;
  }

  const lqf::MetricAssessment* level_assessment = find_metric(report, kRxLevel);
  if (level_assessment == nullptr) {
    std::cerr << "quickstart: signal-power:rx.level is missing from the report\n";
    return 1;
  }
  if (level_assessment->state != lqf::QualityState::Marginal) {
    std::cerr << "quickstart: expected rx.level to be marginal, received "
              << lqf::to_string(level_assessment->state) << "\n";
    return 1;
  }
  if (!level_assessment->value.has_value() || std::fabs(*level_assessment->value - (-8.5)) > 1e-9) {
    std::cerr << "quickstart: expected the newest gauge to be -8.5 dBm\n";
    return 1;
  }

  const lqf::MetricAssessment* counter_assessment = find_metric(report, kCorrectedErrors);
  if (counter_assessment == nullptr) {
    std::cerr << "quickstart: error-counter:errors.corrected is missing from the report\n";
    return 1;
  }
  if (counter_assessment->state != lqf::QualityState::Healthy) {
    std::cerr << "quickstart: expected corrected errors to be healthy, received "
              << lqf::to_string(counter_assessment->state) << "\n";
    return 1;
  }
  if (!counter_assessment->rate_per_second.has_value()) {
    std::cerr << "quickstart: no rate was derived from the counter pair\n";
    return 1;
  }
  if (std::fabs(*counter_assessment->rate_per_second - 0.5) > 1e-9) {
    std::cerr << "quickstart: expected 0.5 corrected errors per second, received "
              << lqf::text::format_double(*counter_assessment->rate_per_second) << "\n";
    return 1;
  }

  // The link-wide roll-up is the worst metric state: marginal beats healthy.
  if (report.overall != lqf::QualityState::Marginal) {
    std::cerr << "quickstart: expected the link to be marginal, received "
              << lqf::to_string(report.overall) << "\n";
    return 1;
  }

  const lqf::Status closed = fabric->close();
  if (!closed.ok()) {
    std::cerr << "quickstart: the fabric did not close cleanly: " << closed.to_text() << "\n";
    return 1;
  }
  return 0;
}
