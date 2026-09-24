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

// lqf benchmark.
//
// EVERY INPUT IS SYNTHETIC. Values are generated from std::mt19937_64 with the
// fixed seed below, so two runs of the same command produce the same digest.
// The rates printed here count operations the library actually completed inside
// this process; they say nothing about hardware.
//
// What is measured:
//   ingest                  observations accepted and visible in Fabric::stats()
//   query                   quality reports actually returned
//   counter-rate            counter deltas with a usable derived rate
//   journal-durability      records made durable with flush() per batch

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "lqf/core/clock.hpp"
#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/counter.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/runtime/fabric.hpp"

namespace {

// Fixed, explicit seed. Changing it changes the generated corpus and therefore
// the digest, which is exactly what makes runs comparable.
constexpr lqf::u64 kSeed = 0x5EED1234C0FFEEULL;

const lqf::MetricId kRxLevel{lqf::MetricFamily::SignalPower, "rx.level"};
const lqf::MetricId kCorrectedErrors{lqf::MetricFamily::ErrorCounter, "errors.corrected"};

constexpr std::size_t kColumnOne = 26;
constexpr std::size_t kColumnTwo = 14;
constexpr std::size_t kColumnThree = 14;
constexpr std::size_t kColumnFour = 16;

struct Options {
  lqf::u64 observations{20000};
  lqf::u64 queries{2000};
  lqf::u64 counters{5000};
  lqf::u64 journal_records{512};
  lqf::u64 batch{64};
  std::string journal{};
};

void print_usage(std::ostream& out) {
  out << "usage: lqf_bench [--observations <n>] [--queries <n>] [--counters <n>]\n"
         "                 [--journal-records <n>] [--batch <n>] [--journal <path>]\n";
}

std::string pad_right(const std::string& text, std::size_t width) {
  if (text.size() >= width) {
    return text;
  }
  return text + std::string(width - text.size(), ' ');
}

std::string pad_left(const std::string& text, std::size_t width) {
  if (text.size() >= width) {
    return text;
  }
  return std::string(width - text.size(), ' ') + text;
}

std::string pad_zero_left(const std::string& text, std::size_t width) {
  if (text.size() >= width) {
    return text;
  }
  return std::string(width - text.size(), '0') + text;
}

// Locale independent fixed point rendering: no stream, no locale, no surprises.
std::string decimal(double value, std::size_t places) {
  std::string zeros(places, '0');
  if (!std::isfinite(value) || value < 0.0) {
    return "0." + zeros;
  }
  double scale = 1.0;
  lqf::u64 unit = 1;
  for (std::size_t index = 0; index < places; ++index) {
    scale *= 10.0;
    unit *= 10U;
  }
  const double scaled = value * scale + 0.5;
  if (scaled >= 1.8e19) {
    return ">1e19";
  }
  const lqf::u64 rounded = static_cast<lqf::u64>(scaled);
  return lqf::text::format_u64(rounded / unit) + "." +
         pad_zero_left(lqf::text::format_u64(rounded % unit), places);
}

double seconds_between(std::chrono::steady_clock::time_point begin,
                       std::chrono::steady_clock::time_point end) {
  const std::chrono::duration<double> elapsed = end - begin;
  return elapsed.count();
}

std::string rate_of(lqf::u64 completed, double seconds) {
  if (seconds <= 0.0) {
    return "n/a";
  }
  return decimal(static_cast<double>(completed) / seconds, 2);
}

void print_row(const std::string& name, lqf::u64 completed, double seconds) {
  std::cout << pad_right(name, kColumnOne) << pad_left(lqf::text::format_u64(completed), kColumnTwo)
            << pad_left(decimal(seconds, 6), kColumnThree) << pad_left(rate_of(completed, seconds), kColumnFour)
            << "\n";
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
  observation.provenance.origin = "lqf-bench/generator";
  observation.provenance.producer = "lqf_bench";
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
  observation.reading = reading;
  observation.observed_at = lqf::Timestamp{observed_nanos, true};
  observation.provenance.transport = lqf::TransportKind::Synthetic;
  observation.provenance.evidence_class = lqf::EvidenceClass::Synthetic;
  observation.provenance.origin = "lqf-bench/generator";
  observation.provenance.producer = "lqf_bench";
  return observation;
}

lqf::CapabilityDeclaration bench_capability(const lqf::LinkIdentity& link,
                                            const lqf::SourceIdentity& source, bool counters) {
  lqf::CapabilityDeclaration declaration;
  declaration.source = source;
  declaration.link_scope = link;
  declaration.revision = lqf::CapabilityRevision(1);
  declaration.transport = lqf::TransportKind::Synthetic;
  declaration.evidence_class = lqf::EvidenceClass::Synthetic;
  declaration.declared_at = lqf::Timestamp{0, false};

  lqf::MetricCapability level;
  level.metric = kRxLevel;
  level.unit = lqf::Unit::DecibelMilliwatt;
  level.semantics = lqf::SampleSemantics::Gauge;
  level.lanes_declared = false;
  level.lane_count = 0;
  declaration.metrics.push_back(level);

  if (counters) {
    lqf::MetricCapability errors;
    errors.metric = kCorrectedErrors;
    errors.unit = lqf::Unit::Count;
    errors.semantics = lqf::SampleSemantics::Counter;
    errors.counter_width = lqf::CounterWidth::Bits64;
    errors.lanes_declared = false;
    errors.lane_count = 0;
    declaration.metrics.push_back(errors);
  }
  return declaration;
}

bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  const auto parse_u64 = [&error](const std::string& text, lqf::u64& out) {
    const lqf::Status status = lqf::text::parse_u64(text, out);
    if (!status.ok()) {
      error = "not an unsigned integer: " + text;
      return false;
    }
    return true;
  };
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    const auto next = [&argc, &argv, &index, &error, &name]() -> const char* {
      if (index + 1 >= argc) {
        error = "option " + name + " requires a value";
        return nullptr;
      }
      ++index;
      return argv[index];
    };
    if (name == "--help") {
      print_usage(std::cout);
      return false;
    } else if (name == "--observations") {
      const char* value = next();
      if (value == nullptr || !parse_u64(value, options.observations)) {
        return false;
      }
    } else if (name == "--queries") {
      const char* value = next();
      if (value == nullptr || !parse_u64(value, options.queries)) {
        return false;
      }
    } else if (name == "--counters") {
      const char* value = next();
      if (value == nullptr || !parse_u64(value, options.counters)) {
        return false;
      }
    } else if (name == "--journal-records") {
      const char* value = next();
      if (value == nullptr || !parse_u64(value, options.journal_records)) {
        return false;
      }
    } else if (name == "--batch") {
      const char* value = next();
      if (value == nullptr || !parse_u64(value, options.batch)) {
        return false;
      }
    } else if (name == "--journal") {
      const char* value = next();
      if (value == nullptr) {
        return false;
      }
      options.journal = value;
    } else {
      error = "unknown option: " + name;
      return false;
    }
  }
  if (options.batch == 0) {
    error = "--batch must be at least 1";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    if (!error.empty()) {
      std::cerr << "lqf_bench: " << error << "\n";
      print_usage(std::cerr);
      return 2;
    }
    return 0;  // --help
  }

  std::mt19937_64 generator(kSeed);
  std::uniform_real_distribution<double> level_distribution(-12.0, -4.0);

  std::cout << "lqf benchmark: seed=0x" << lqf::text::hex_u64(kSeed) << "\n";
  std::cout << "every input below is synthetic (generated by std::mt19937_64 with that fixed\n";
  std::cout << "seed); the rates count completed work inside this process and say nothing\n";
  std::cout << "about hardware.\n\n";

  const auto clock = std::make_shared<lqf::ManualClock>();
  const lqf::LinkIdentity link{lqf::LinkId(std::string("bench-link")), lqf::LinkGeneration(1)};
  const lqf::SourceIdentity source{lqf::SourceId(std::string("bench-source")),
                                   lqf::SourceIncarnation(1)};

  std::cout << pad_right("benchmark", kColumnOne) << pad_left("completed", kColumnTwo)
            << pad_left("seconds", kColumnThree) << pad_left("ops/second", kColumnFour) << "\n";
  std::cout << std::string(kColumnOne + kColumnTwo + kColumnThree + kColumnFour, '-') << "\n";

  // -------------------------------------------------------------------------
  // The in-memory fabric: ingest, query and counter rate derivation.
  // -------------------------------------------------------------------------
  lqf::FabricConfig config;
  config.clock = clock;
  config.journal.enabled = false;
  lqf::Outcome<std::shared_ptr<lqf::Fabric>> opened = lqf::Fabric::open(config);
  if (!opened.ok()) {
    std::cerr << "lqf_bench: the fabric could not be opened: " << opened.status().to_text() << "\n";
    return 1;
  }
  const std::shared_ptr<lqf::Fabric>& fabric = opened.value();
  const lqf::Outcome<lqf::CapabilityAck> declared =
      fabric->declare_capability(bench_capability(link, source, true));
  if (!declared.ok()) {
    std::cerr << "lqf_bench: the capability was refused: " << declared.status().to_text() << "\n";
    return 1;
  }

  // (a) ingest throughput, counted from Fabric::stats() after the loop.
  lqf::u64 sequence = 1;
  const lqf::u64 accepted_before = fabric->stats().observations_accepted;
  const auto ingest_begin = std::chrono::steady_clock::now();
  for (lqf::u64 done = 0; done < options.observations;) {
    std::vector<lqf::Observation> batch;
    const lqf::u64 remaining = options.observations - done;
    const lqf::u64 take = remaining < options.batch ? remaining : options.batch;
    batch.reserve(static_cast<std::size_t>(take));
    for (lqf::u64 index = 0; index < take; ++index) {
      batch.push_back(gauge_observation(link, source, sequence, level_distribution(generator),
                                        clock->now().wall_nanos));
      sequence += 1;
    }
    const lqf::Outcome<lqf::BatchOutcome> outcome = fabric->ingest_batch(batch);
    if (!outcome.ok()) {
      std::cerr << "lqf_bench: a batch was refused: " << outcome.status().to_text() << "\n";
      return 1;
    }
    done += take;
  }
  const auto ingest_end = std::chrono::steady_clock::now();
  const lqf::FabricStats after_ingest = fabric->stats();
  const lqf::u64 ingested = after_ingest.observations_accepted - accepted_before;
  print_row("ingest-observations", ingested, seconds_between(ingest_begin, ingest_end));
  std::cout << "  fabric stats: accepted=" << lqf::text::format_u64(after_ingest.observations_accepted)
            << " duplicates=" << lqf::text::format_u64(after_ingest.observations_duplicate)
            << " reordered=" << lqf::text::format_u64(after_ingest.observations_reordered)
            << " rejected=" << lqf::text::format_u64(after_ingest.observations_rejected)
            << " streams=" << lqf::text::format_u64(static_cast<lqf::u64>(after_ingest.streams))
            << " retained=" << lqf::text::format_u64(static_cast<lqf::u64>(after_ingest.retained_records))
            << "\n";
  if (ingested != options.observations) {
    std::cerr << "lqf_bench: " << lqf::text::format_u64(options.observations)
              << " observations were generated but " << lqf::text::format_u64(ingested)
              << " are visible in the stats; the throughput figure would be dishonest\n";
    return 1;
  }

  // (b) query throughput: only reports actually returned are counted.
  lqf::QualityQuery query;
  query.link = link;
  query.include_evidence = false;
  lqf::u64 reports = 0;
  lqf::u64 digest = lqf::fnv1a64(std::string_view("lqf-bench"));
  const auto query_begin = std::chrono::steady_clock::now();
  for (lqf::u64 index = 0; index < options.queries; ++index) {
    const lqf::Outcome<lqf::LinkQualityReport> report = fabric->query(query);
    if (!report.ok()) {
      std::cerr << "lqf_bench: a query failed: " << report.status().to_text() << "\n";
      return 1;
    }
    digest = lqf::fnv1a64(std::string_view(render_quality_state(report.value().overall)),
                          digest);
    for (const lqf::MetricAssessment& assessment : report.value().metrics) {
      digest = lqf::fnv1a64(std::string_view(lqf::to_string(assessment.state)), digest);
    }
    reports += 1;
  }
  const auto query_end = std::chrono::steady_clock::now();
  print_row("query-reports", reports, seconds_between(query_begin, query_end));

  // (c) counter rate derivation: each pair of readings is one derivation, and
  // only the ones the library reports as having a usable rate are counted.
  lqf::u64 counter_value = 1000;
  lqf::u64 derivations = 0;
  lqf::u64 last_delta = 0;
  const auto counter_begin = std::chrono::steady_clock::now();
  for (lqf::u64 index = 0; index < options.counters; ++index) {
    const lqf::i64 first_nanos = clock->now().wall_nanos;
    const lqf::Outcome<lqf::IngestOutcome> baseline =
        fabric->ingest(counter_observation(link, source, sequence, counter_value, first_nanos));
    if (!baseline.ok()) {
      std::cerr << "lqf_bench: a counter baseline was refused: " << baseline.status().to_text()
                << "\n";
      return 1;
    }
    sequence += 1;
    clock->advance_seconds(1);
    const lqf::u64 delta = 1U + (generator() % 1000U);
    counter_value += delta;
    const lqf::Outcome<lqf::IngestOutcome> advanced = fabric->ingest(
        counter_observation(link, source, sequence, counter_value, clock->now().wall_nanos));
    if (!advanced.ok()) {
      std::cerr << "lqf_bench: a counter reading was refused: " << advanced.status().to_text()
                << "\n";
      return 1;
    }
    sequence += 1;
    if (advanced.value().rate_admissible && advanced.value().counter_event == lqf::CounterEvent::Advance) {
      derivations += 1;
      last_delta = delta;
    }
  }
  const auto counter_end = std::chrono::steady_clock::now();
  print_row("counter-rate-derivations", derivations, seconds_between(counter_begin, counter_end));

  // The derived rate is checked once against the generated delta, so the number
  // above counts derivations that produced a usable value.
  query.metrics = std::vector<lqf::MetricId>{kCorrectedErrors};
  const lqf::Outcome<lqf::LinkQualityReport> counter_report = fabric->query(query);
  if (!counter_report.ok()) {
    std::cerr << "lqf_bench: the counter verification query failed: "
              << counter_report.status().to_text() << "\n";
    return 1;
  }
  double derived_rate = -1.0;
  for (const lqf::MetricAssessment& assessment : counter_report.value().metrics) {
    if (assessment.metric == kCorrectedErrors && assessment.rate_per_second.has_value()) {
      derived_rate = *assessment.rate_per_second;
    }
  }
  if (std::fabs(derived_rate - static_cast<double>(last_delta)) > 1e-9) {
    std::cerr << "lqf_bench: the derived counter rate " << decimal(derived_rate, 3)
              << " does not match the generated delta " << lqf::text::format_u64(last_delta)
              << " per second; the derivation rate above would be meaningless\n";
    return 1;
  }
  std::cout << "  verified last derived rate " << decimal(derived_rate, 3)
            << " per second against the generated delta "
            << lqf::text::format_u64(last_delta) << " over one second\n";

  std::cout << "digest fnv1a64=0x" << lqf::text::hex_u64(digest)
            << " over " << lqf::text::format_u64(reports) << " produced report states\n";

  // -------------------------------------------------------------------------
  // (d) journal durability: records made durable with flush() per batch.
  // -------------------------------------------------------------------------
  std::filesystem::path journal_path;
  bool remove_journal = false;
  if (options.journal.empty()) {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    journal_path = std::filesystem::temp_directory_path() /
                   ("lqf-bench-" + lqf::text::format_i64(static_cast<lqf::i64>(stamp)) + ".journal");
    remove_journal = true;
  } else {
    journal_path = options.journal;
  }

  lqf::FabricConfig journal_config;
  journal_config.clock = clock;
  journal_config.journal.enabled = true;
  journal_config.journal.path = journal_path.string();
  lqf::Outcome<std::shared_ptr<lqf::Fabric>> opened_journal = lqf::Fabric::open(journal_config);
  if (!opened_journal.ok()) {
    std::cerr << "lqf_bench: the journal backed fabric could not be opened: "
              << opened_journal.status().to_text() << "\n";
    return 1;
  }
  const std::shared_ptr<lqf::Fabric>& journal_fabric = opened_journal.value();
  const lqf::Outcome<lqf::CapabilityAck> journal_declared =
      journal_fabric->declare_capability(bench_capability(link, source, false));
  if (!journal_declared.ok()) {
    std::cerr << "lqf_bench: the capability was refused: " << journal_declared.status().to_text()
              << "\n";
    return 1;
  }

  const lqf::u64 written_before = journal_fabric->journal_stats().records_written;
  lqf::u64 durable = 0;
  lqf::u64 flushes = 0;
  lqf::u64 journal_sequence = 1;
  const auto journal_begin = std::chrono::steady_clock::now();
  for (lqf::u64 done = 0; done < options.journal_records;) {
    std::vector<lqf::Observation> batch;
    const lqf::u64 remaining = options.journal_records - done;
    const lqf::u64 take = remaining < options.batch ? remaining : options.batch;
    batch.reserve(static_cast<std::size_t>(take));
    for (lqf::u64 index = 0; index < take; ++index) {
      batch.push_back(gauge_observation(link, source, journal_sequence, level_distribution(generator),
                                        clock->now().wall_nanos));
      journal_sequence += 1;
    }
    const lqf::Outcome<lqf::BatchOutcome> outcome = journal_fabric->ingest_batch(batch);
    if (!outcome.ok()) {
      std::cerr << "lqf_bench: a journal batch was refused: " << outcome.status().to_text() << "\n";
      return 1;
    }
    const lqf::Status flushed = journal_fabric->flush();
    if (!flushed.ok()) {
      std::cerr << "lqf_bench: flush failed: " << flushed.to_text() << "\n";
      return 1;
    }
    flushes += 1;
    done += take;
  }
  const auto journal_end = std::chrono::steady_clock::now();
  const lqf::JournalStats journal_stats = journal_fabric->journal_stats();
  const lqf::FabricStats journal_fabric_stats = journal_fabric->stats();
  durable = journal_stats.records_written - written_before;
  print_row("journal-durable-records", durable, seconds_between(journal_begin, journal_end));
  std::cout << "  ingest stats: accepted="
            << lqf::text::format_u64(journal_fabric_stats.observations_accepted)
            << " duplicates=" << lqf::text::format_u64(journal_fabric_stats.observations_duplicate)
            << " reordered=" << lqf::text::format_u64(journal_fabric_stats.observations_reordered)
            << " rejected=" << lqf::text::format_u64(journal_fabric_stats.observations_rejected)
            << " journal-rejections="
            << lqf::text::format_u64(journal_fabric_stats.journal_rejections) << "\n";
  std::cout << "  journal stats: flushes=" << lqf::text::format_u64(flushes)
            << " records-written=" << lqf::text::format_u64(journal_stats.records_written)
            << " rejected-appends=" << lqf::text::format_u64(journal_stats.rejected_appends)
            << " queue-depth=" << lqf::text::format_u64(static_cast<lqf::u64>(journal_stats.queue_depth))
            << "\n";
  if (journal_stats.rejected_appends != 0) {
    std::cerr << "lqf_bench: the journal refused appends, so durability was degraded and the "
                 "durability figure above does not describe completed work\n";
    return 1;
  }
  if (durable < options.journal_records) {
    std::cerr << "lqf_bench: " << lqf::text::format_u64(options.journal_records)
              << " observations were ingested but only " << lqf::text::format_u64(durable)
              << " records were written\n";
    return 1;
  }

  const lqf::Status journal_closed = journal_fabric->close();
  if (!journal_closed.ok()) {
    std::cerr << "lqf_bench: the journal backed fabric did not close cleanly: "
              << journal_closed.to_text() << "\n";
    return 1;
  }
  const lqf::Status closed = fabric->close();
  if (!closed.ok()) {
    std::cerr << "lqf_bench: the fabric did not close cleanly: " << closed.to_text() << "\n";
    return 1;
  }
  if (remove_journal) {
    std::error_code removal_error;
    std::filesystem::remove(journal_path, removal_error);
    if (removal_error) {
      std::cerr << "lqf_bench: the temporary journal could not be removed: "
                << removal_error.message() << "\n";
    }
  }

  std::cout << "\nall benchmark input is synthetic; the rates above measure completed library work "
               "in this\nprocess and are not a claim about hardware.\n";
  return 0;
}
