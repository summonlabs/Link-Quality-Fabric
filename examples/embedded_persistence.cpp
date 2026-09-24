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

// Embedded persistence: ingest into a journal backed fabric, flush, close,
// reopen, and show what recovery actually proves.
//
// Two things are checked, and both are properties of the library rather than of
// this example: recovered evidence keeps the observation time the source
// supplied, and recovered evidence is never fresh, so the state after a restart
// is STALE and can never be HEALTHY.

#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "lqf/core/clock.hpp"
#include "lqf/core/text.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/persist/journal.hpp"
#include "lqf/runtime/fabric.hpp"

namespace {

const lqf::MetricId kRxLevel{lqf::MetricFamily::SignalPower, "rx.level"};

void print_recovery(const lqf::RecoveryReport& report) {
  std::cout << "recovery\n";
  std::cout << "  opened " << (report.opened ? "true" : "false") << "\n";
  std::cout << "  header-valid " << (report.header_valid ? "true" : "false") << "\n";
  std::cout << "  records-read " << lqf::text::format_u64(report.records_read) << "\n";
  std::cout << "  bytes-read " << lqf::text::format_u64(report.bytes_read) << "\n";
  std::cout << "  torn-tail " << (report.torn_tail ? "true" : "false") << "\n";
  std::cout << "  degraded " << (report.degraded ? "true" : "false") << "\n";
  std::cout << "  snapshot-loaded " << (report.snapshot_loaded ? "true" : "false") << "\n";
  std::cout << "  status " << report.status.to_text() << "\n";
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
  observation.provenance.origin = "embedded-persistence/generator";
  observation.provenance.producer = "lqf-example-embedded-persistence";
  return observation;
}

lqf::CapabilityDeclaration level_capability(const lqf::LinkIdentity& link,
                                            const lqf::SourceIdentity& source) {
  lqf::CapabilityDeclaration declaration;
  declaration.source = source;
  declaration.link_scope = link;
  declaration.revision = lqf::CapabilityRevision(1);
  declaration.transport = lqf::TransportKind::Synthetic;
  declaration.evidence_class = lqf::EvidenceClass::Synthetic;
  declaration.declared_at = lqf::Timestamp{1'700'000'000'000'000'000LL, true};
  lqf::MetricCapability capability;
  capability.metric = kRxLevel;
  capability.unit = lqf::Unit::DecibelMilliwatt;
  capability.semantics = lqf::SampleSemantics::Gauge;
  capability.lanes_declared = false;
  capability.lane_count = 0;
  declaration.metrics.push_back(capability);
  return declaration;
}

}  // namespace

int main() {
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("lqf-embedded-persistence-" + lqf::text::format_i64(static_cast<lqf::i64>(stamp)));
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::cerr << "embedded-persistence: the temporary directory could not be created: "
              << error.message() << "\n";
    return 1;
  }
  const std::string journal = (directory / "evidence.journal").string();

  const auto clock = std::make_shared<lqf::ManualClock>();
  const lqf::LinkIdentity link{lqf::LinkId(std::string("embedded-link")),
                               lqf::LinkGeneration(1)};
  const lqf::SourceIdentity source{lqf::SourceId(std::string("embedded-source")),
                                   lqf::SourceIncarnation(1)};

  // sequence -> the observation time the source supplied for it.
  std::vector<std::pair<lqf::u64, lqf::i64>> supplied;

  {
    lqf::FabricConfig config;
    config.clock = clock;
    config.journal.enabled = true;
    config.journal.path = journal;
    lqf::Outcome<std::shared_ptr<lqf::Fabric>> opened = lqf::Fabric::open(config);
    if (!opened.ok()) {
      std::cerr << "embedded-persistence: the fabric could not be opened: "
                << opened.status().to_text() << "\n";
      return 1;
    }
    const std::shared_ptr<lqf::Fabric>& fabric = opened.value();

    const lqf::Outcome<lqf::CapabilityAck> declared =
        fabric->declare_capability(level_capability(link, source));
    if (!declared.ok()) {
      std::cerr << "embedded-persistence: the capability was refused: "
                << declared.status().to_text() << "\n";
      return 1;
    }

    for (lqf::u64 index = 0; index < 3U; ++index) {
      const lqf::i64 observed = clock->now().wall_nanos;
      const lqf::Observation observation =
          gauge_observation(link, source, index + 1U, -6.0 - static_cast<double>(index), observed);
      const lqf::Outcome<lqf::IngestOutcome> outcome = fabric->ingest(observation);
      if (!outcome.ok()) {
        std::cerr << "embedded-persistence: evidence was refused: " << outcome.status().to_text()
                  << "\n";
        return 1;
      }
      std::cout << "ingested seq=" << lqf::text::format_u64(index + 1U)
                << " observed=" << lqf::text::format_i64(observed) << "\n";
      supplied.emplace_back(index + 1U, observed);
      clock->advance_seconds(1);
    }

    const lqf::Status flushed = fabric->flush();
    if (!flushed.ok()) {
      std::cerr << "embedded-persistence: flush failed: " << flushed.to_text() << "\n";
      return 1;
    }
    const lqf::Status closed = fabric->close();
    if (!closed.ok()) {
      std::cerr << "embedded-persistence: close failed: " << closed.to_text() << "\n";
      return 1;
    }
  }

  // Reopen the same journal. Every record now arrives as recovered evidence.
  lqf::FabricConfig config;
  config.clock = clock;
  config.journal.enabled = true;
  config.journal.path = journal;
  lqf::Outcome<std::shared_ptr<lqf::Fabric>> reopened = lqf::Fabric::open(config);
  if (!reopened.ok()) {
    std::cerr << "embedded-persistence: the journal could not be recovered: "
              << reopened.status().to_text() << "\n";
    return 1;
  }
  const std::shared_ptr<lqf::Fabric>& fabric = reopened.value();
  print_recovery(fabric->recovery_report());

  if (fabric->recovery_report().records_read == 0U) {
    std::cerr << "embedded-persistence: recovery read no records from the journal\n";
    return 1;
  }
  if (fabric->recovery_report().torn_tail || fabric->recovery_report().degraded) {
    std::cerr << "embedded-persistence: recovery did not complete cleanly\n";
    return 1;
  }

  lqf::WindowQuery window;
  window.link = link;
  window.axis = lqf::WindowAxis::ObservationTime;
  window.from_nanos = std::numeric_limits<lqf::i64>::min();
  window.to_nanos = std::numeric_limits<lqf::i64>::max();
  window.limit = 16;
  const lqf::Outcome<lqf::WindowResult> recovered = fabric->window(window);
  if (!recovered.ok()) {
    std::cerr << "embedded-persistence: the window query failed: " << recovered.status().to_text()
              << "\n";
    return 1;
  }
  if (recovered.value().returned != supplied.size()) {
    std::cerr << "embedded-persistence: expected " << supplied.size()
              << " recovered records, received " << recovered.value().returned << "\n";
    return 1;
  }
  for (const lqf::EvidenceRecord& record : recovered.value().records) {
    lqf::i64 expected = 0;
    bool found = false;
    for (const std::pair<lqf::u64, lqf::i64>& entry : supplied) {
      if (entry.first == record.observation.sequence.value()) {
        expected = entry.second;
        found = true;
        break;
      }
    }
    if (!found) {
      std::cerr << "embedded-persistence: a recovered record has an unknown sequence\n";
      return 1;
    }
    if (record.observation.observed_at.unix_nanos != expected) {
      std::cerr << "embedded-persistence: recovered sequence "
                << lqf::text::format_u64(record.observation.sequence.value())
                << " was re-stamped: expected " << lqf::text::format_i64(expected)
                << ", recovered " << lqf::text::format_i64(record.observation.observed_at.unix_nanos)
                << "\n";
      return 1;
    }
    std::cout << "recovered seq=" << lqf::text::format_u64(record.observation.sequence.value())
              << " observed=" << lqf::text::format_i64(record.observation.observed_at.unix_nanos)
              << " origin=" << lqf::to_string(record.origin) << "\n";
  }

  lqf::QualityQuery query;
  query.link = link;
  query.include_evidence = true;
  const lqf::Outcome<lqf::LinkQualityReport> queried = fabric->query(query);
  if (!queried.ok()) {
    std::cerr << "embedded-persistence: the query failed: " << queried.status().to_text() << "\n";
    return 1;
  }
  const lqf::LinkQualityReport& report = queried.value();
  std::cout << lqf::render_report(report);

  if (report.overall == lqf::QualityState::Healthy) {
    std::cerr << "embedded-persistence: recovered evidence was reported as healthy\n";
    return 1;
  }
  if (report.overall != lqf::QualityState::Stale) {
    std::cerr << "embedded-persistence: expected the recovered state to be stale, received "
              << lqf::to_string(report.overall) << "\n";
    return 1;
  }
  // Nothing that came back from the journal may be counted as fresh: recovered
  // evidence is fenced by the process incarnation that received it.
  for (const lqf::MetricAssessment& assessment : report.metrics) {
    for (const lqf::EvidenceRef& reference : assessment.evidence) {
      if (reference.fresh) {
        std::cerr << "embedded-persistence: recovered evidence for "
                  << lqf::render_metric_id(assessment.metric) << " is reported as fresh\n";
        return 1;
      }
    }
  }

  const lqf::Status closed = fabric->close();
  if (!closed.ok()) {
    std::cerr << "embedded-persistence: the fabric did not close cleanly: " << closed.to_text()
              << "\n";
    return 1;
  }
  std::filesystem::remove_all(directory, error);
  if (error) {
    std::cerr << "embedded-persistence: the temporary directory could not be removed: "
              << error.message() << "\n";
  }
  std::cout << "state after reopen is " << lqf::to_string(report.overall)
            << " and the recovered observation times are unchanged\n";
  return 0;
}
