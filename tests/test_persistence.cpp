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

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const MetricId kErrors(MetricFamily::ErrorCounter, "errors.uncorrectable");

struct Persisted {
  std::shared_ptr<Fabric> fabric{};
  std::shared_ptr<ManualClock> clock{};
};

Outcome<Persisted> open_persisted(const std::string& path, i64 wall_nanos = 1'700'000'000'000'000'000LL) {
  FabricConfig config;
  config.clock = std::make_shared<ManualClock>(wall_nanos, 0);
  config.forced_epoch = FabricEpoch(0xA1B2C3ULL);
  config.journal.enabled = true;
  config.journal.path = path;
  config.journal.fsync_on_flush = true;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  if (!fabric.ok()) {
    return fabric.status();
  }
  Persisted persisted;
  persisted.fabric = fabric.value();
  persisted.clock = std::static_pointer_cast<ManualClock>(config.clock);
  return persisted;
}

Outcome<Persisted> open_persisted_with_epoch(const std::string& path, u64 epoch) {
  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(epoch);
  config.journal.enabled = true;
  config.journal.path = path;
  config.journal.fsync_on_flush = true;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  if (!fabric.ok()) {
    return fabric.status();
  }
  Persisted persisted;
  persisted.fabric = fabric.value();
  persisted.clock = std::static_pointer_cast<ManualClock>(config.clock);
  return persisted;
}

std::string read_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return content;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

u64 file_size(const std::string& path) {
  Outcome<u64> size = lqf::file_size_bytes(path);
  return size.ok() ? size.value() : 0;
}

}  // namespace

LQF_TEST(persistence, recovered_evidence_keeps_its_times_and_is_never_fresh) {
  TempDir directory("persist");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-p")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-p")), SourceIncarnation(1));
  const i64 observed = 1'699'999'999'000'000'000LL;

  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel})));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->ingest(
        make_gauge(link, source, kRxLevel, -3.0, 1, observed, Unit::DecibelMilliwatt)));
    QualityQuery query;
    query.link = link;
    LQF_CHECK(persisted.value().fabric->query(query).value().overall == QualityState::Healthy);
    LQF_CHECK_STATUS_OK(persisted.value().fabric->flush());
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }

  // A new process incarnation recovers the same evidence.
  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0xFEEDFACEULL);
  LQF_CHECK_STATUS_OK(reopened.status());
  LQF_CHECK_EQ(reopened.value().fabric->recovery_report().records_read > 0, true);
  LQF_CHECK(!reopened.value().fabric->recovery_report().degraded);

  QualityQuery query;
  query.link = link;
  Outcome<LinkQualityReport> report = reopened.value().fabric->query(query);
  LQF_CHECK(report.ok());
  // Recovered evidence can never assert current health.
  LQF_CHECK(report.value().overall == QualityState::Stale);
  LQF_CHECK(has_reason(report.value().reasons, ReasonCode::RecoveredEvidenceOnly));
  LQF_CHECK_EQ(report.value().evidence_fresh, u64{0});
  LQF_CHECK_EQ(report.value().evidence_recovered, u64{1});

  WindowQuery window;
  window.link = link;
  window.limit = 16;
  Outcome<WindowResult> records = reopened.value().fabric->query(query).ok()
                                      ? reopened.value().fabric->window(window)
                                      : reopened.value().fabric->window(window);
  LQF_CHECK(records.ok());
  LQF_CHECK_EQ(records.value().records.size(), std::size_t{1});
  // The original observation time survives the round trip unchanged.
  LQF_CHECK_EQ(records.value().records[0].observation.observed_at.unix_nanos, observed);
  LQF_CHECK(records.value().records[0].origin == EvidenceOrigin::Recovered);

  // Fresh live evidence restores the classification.
  LQF_CHECK_STATUS_OK(reopened.value().fabric->ingest(make_gauge(
      link, source, kRxLevel, -3.0, 2, reopened.value().clock->now().wall_nanos,
      Unit::DecibelMilliwatt)));
  Outcome<LinkQualityReport> refreshed = reopened.value().fabric->query(query);
  LQF_CHECK(refreshed.value().overall == QualityState::Healthy);
  // The conclusion now rests on live evidence only; the recovered record is
  // still retained and still visible in the historical window.
  LQF_CHECK_EQ(refreshed.value().evidence_recovered, u64{0});
  LQF_CHECK(refreshed.value().evidence_fresh >= 1);
  Outcome<WindowResult> retained = reopened.value().fabric->window(window);
  LQF_CHECK(retained.ok());
  bool saw_recovered = false;
  for (const EvidenceRecord& record : retained.value().records) {
    if (record.origin == EvidenceOrigin::Recovered) {
      saw_recovered = true;
    }
  }
  LQF_CHECK(saw_recovered);
  LQF_CHECK_STATUS_OK(reopened.value().fabric->close());
}

LQF_TEST(persistence, replayed_evidence_is_refused_after_a_restart) {
  TempDir directory("replay");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-r")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-r")), SourceIncarnation(1));
  const Observation observation =
      make_gauge(link, source, kRxLevel, -3.0, 7, 1'700'000'000'000'000'000LL,
                 Unit::DecibelMilliwatt);

  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel})));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->ingest(observation));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0x55AA55AAULL);
  LQF_CHECK_STATUS_OK(reopened.status());
  // The verification window was persisted with the evidence, so the replay is
  // refused as a duplicate exactly as it was before the restart.
  Outcome<IngestOutcome> replayed = reopened.value().fabric->ingest(observation);
  LQF_CHECK(replayed.ok());
  LQF_CHECK(replayed.value().duplicate);
  LQF_CHECK(!replayed.value().accepted);

  // The same sequence with different content is an identity mismatch.
  Observation tampered = observation;
  std::get<GaugeReading>(tampered.reading).value = -9.0;
  const Outcome<IngestOutcome> mismatch = reopened.value().fabric->ingest(tampered);
  LQF_CHECK(!mismatch.ok());
  LQF_CHECK(mismatch.status().code() == StatusCode::IdMismatch);
  LQF_CHECK_STATUS_OK(reopened.value().fabric->close());
}

LQF_TEST(persistence, truncated_tail_is_repaired_conservatively) {
  TempDir directory("truncate");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-t")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-t")), SourceIncarnation(1));

  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel})));
    for (u64 index = 0; index < 3; ++index) {
      LQF_CHECK_STATUS_OK(persisted.value().fabric->ingest(
          make_gauge(link, source, kRxLevel, -3.0 - static_cast<double>(index), index + 1,
                     1'700'000'000'000'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                     Unit::DecibelMilliwatt)));
    }
    LQF_CHECK_STATUS_OK(persisted.value().fabric->flush());
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }

  // Damage the file exactly as an interrupted write would: a truncated record
  // at the end of the file.
  const std::string intact = read_file(path);
  LQF_CHECK(intact.size() > 32);
  write_file(path, intact.substr(0, intact.size() - 6));

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0x11112222ULL);
  LQF_CHECK_STATUS_OK(reopened.status());
  const RecoveryReport& report = reopened.value().fabric->recovery_report();
  LQF_CHECK(report.torn_tail);
  LQF_CHECK(report.truncated_file);
  LQF_CHECK(report.bytes_discarded > 0);
  LQF_CHECK(report.records_read >= 4);
  // The repaired file is smaller than the damaged one and no longer torn.
  const u64 repaired_size = file_size(path);
  LQF_CHECK(repaired_size <= intact.size());
  // The report keeps the finding (a torn tail was seen) and records that the
  // file was repaired; the recovered state is a verified prefix, not a guess.
  LQF_CHECK(reopened.value().fabric->stats().recovery.torn_tail);
  LQF_CHECK(reopened.value().fabric->stats().recovery.truncated);
  LQF_CHECK_STATUS_OK(reopened.value().fabric->close());

  // A second reopen sees a clean file.
  Outcome<Persisted> clean = open_persisted_with_epoch(path, 0x33334444ULL);
  LQF_CHECK_STATUS_OK(clean.status());
  LQF_CHECK(!clean.value().fabric->recovery_report().torn_tail);
  LQF_CHECK(!clean.value().fabric->recovery_report().degraded);
  LQF_CHECK_STATUS_OK(clean.value().fabric->close());
}

LQF_TEST(persistence, corrupted_record_is_rejected_not_guessed) {
  TempDir directory("corrupt");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-c")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-c")), SourceIncarnation(1));

  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel})));
    for (u64 index = 0; index < 4; ++index) {
      LQF_CHECK_STATUS_OK(persisted.value().fabric->ingest(
          make_gauge(link, source, kRxLevel, -3.0, index + 1,
                     1'700'000'000'000'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                     Unit::DecibelMilliwatt)));
    }
    LQF_CHECK_STATUS_OK(persisted.value().fabric->flush());
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }

  // Flip a byte inside the record area: the checksum must catch it.
  std::string damaged = read_file(path);
  LQF_CHECK(damaged.size() > 60);
  damaged[50] = static_cast<char>(damaged[50] ^ 0x5A);
  write_file(path, damaged);

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0x9999AAAAULL);
  LQF_CHECK(!reopened.ok());
  LQF_CHECK(reopened.status().code() == StatusCode::Corrupt);
  LQF_CHECK(!reopened.status().message().empty());
}

LQF_TEST(persistence, corrupted_header_is_refused_outright) {
  TempDir directory("header");
  const std::string path = directory.file("state.lqf");
  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }
  std::string damaged = read_file(path);
  LQF_CHECK(damaged.size() >= 40);
  damaged[2] = 'X';
  write_file(path, damaged);

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0x77778888ULL);
  LQF_CHECK(!reopened.ok());
  LQF_CHECK(reopened.status().code() == StatusCode::Corrupt);
}

LQF_TEST(persistence, interrupted_compaction_temp_file_is_discarded) {
  TempDir directory("tempfile");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-x")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-x")), SourceIncarnation(1));
  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel})));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->ingest(
        make_gauge(link, source, kRxLevel, -3.0, 1, 1'700'000'000'000'000'000LL,
                   Unit::DecibelMilliwatt)));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }
  // A leftover temporary file is never authoritative.
  write_file(path + ".tmp", "this was never a complete journal");

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0xABCDEF01ULL);
  LQF_CHECK_STATUS_OK(reopened.status());
  LQF_CHECK(reopened.value().fabric->recovery_report().temp_discarded);
  LQF_CHECK(!file_exists(path + ".tmp"));
  LQF_CHECK_STATUS_OK(reopened.value().fabric->close());
}

LQF_TEST(persistence, compaction_preserves_state_and_bounds_growth) {
  TempDir directory("compact");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-k")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-k")), SourceIncarnation(1));

  u64 accepted = 0;
  {
    Outcome<Persisted> persisted = open_persisted(path);
    LQF_CHECK_STATUS_OK(persisted.status());
    LQF_CHECK_STATUS_OK(
        persisted.value().fabric->declare_capability(make_capability(link, source, {kRxLevel, kErrors})));
    for (u64 index = 0; index < 200; ++index) {
      const Outcome<IngestOutcome> gauge = persisted.value().fabric->ingest(
          make_gauge(link, source, kRxLevel, -3.0, index + 1,
                     1'700'000'000'000'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                     Unit::DecibelMilliwatt));
      LQF_CHECK(gauge.ok());
      const Outcome<IngestOutcome> counter = persisted.value().fabric->ingest(
          make_counter(link, source, kErrors, index, index + 1,
                       1'700'000'000'000'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                       Unit::Count));
      LQF_CHECK(counter.ok());
      accepted += 2;
    }
    LQF_CHECK_STATUS_OK(persisted.value().fabric->flush());
    const u64 before = file_size(path);
    LQF_CHECK(before > 0);
    QualityQuery query;
    query.link = link;
    const LinkQualityReport report_before = persisted.value().fabric->query(query).value();
    LQF_CHECK_STATUS_OK(persisted.value().fabric->compact());
    const u64 after = file_size(path);
    LQF_CHECK(after < before);
    // The state after compaction is identical to the state before it.
    const LinkQualityReport report_after = persisted.value().fabric->query(query).value();
    LQF_CHECK_EQ(render_report(report_before), render_report(report_after));
    LQF_CHECK_STATUS_OK(persisted.value().fabric->close());
  }

  Outcome<Persisted> reopened = open_persisted_with_epoch(path, 0x0BADF00DULL);
  LQF_CHECK_STATUS_OK(reopened.status());
  LQF_CHECK(reopened.value().fabric->recovery_report().snapshot_loaded);
  QualityQuery query;
  query.link = link;
  LQF_CHECK(reopened.value().fabric->query(query).value().overall == QualityState::Stale);
  WindowQuery window;
  window.link = link;
  window.limit = 4096;
  Outcome<WindowResult> records = reopened.value().fabric->window(window);
  LQF_CHECK(records.ok());
  LQF_CHECK(records.value().records.size() > 0);
  // Every recovered record is recovered, none claims to be live.
  for (const EvidenceRecord& record : records.value().records) {
    LQF_CHECK(record.origin == EvidenceOrigin::Recovered);
  }
  LQF_CHECK_STATUS_OK(reopened.value().fabric->close());
}

LQF_TEST(persistence, disabled_persistence_is_refused_explicitly) {
  FabricConfig config;
  config.forced_epoch = FabricEpoch(5);
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());
  LQF_CHECK(!config.journal.enabled);
  LQF_CHECK(fabric.value()->flush().code() == StatusCode::Refused);
  LQF_CHECK(fabric.value()->compact().code() == StatusCode::Refused);
  LQF_CHECK(!fabric.value()->stats().journal_enabled);
  LQF_CHECK_STATUS_OK(fabric.value()->close());
}
