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

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "lqf/core/lock_tracker.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const MetricId kErrors(MetricFamily::ErrorCounter, "errors.uncorrectable");

}  // namespace

LQF_TEST(concurrency, concurrent_ingest_and_query_keep_the_invariants) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();

  constexpr std::size_t kIngestThreads = 4;
  constexpr std::size_t kPerThread = 150;
  constexpr std::size_t kQueryThreads = 3;

  std::vector<SourceIdentity> sources;
  for (std::size_t index = 0; index < kIngestThreads; ++index) {
    SourceIdentity source(SourceId(std::string("src-") + text::format_u64(index)),
                          SourceIncarnation(1));
    sources.push_back(source);
    LQF_CHECK(self.fabric->declare_capability(make_capability(self.link, source, {kRxLevel}))
                  .ok());
  }

  std::atomic<bool> stop_queries{false};
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> rejected{0};
  std::vector<std::string> failures(kIngestThreads + kQueryThreads);

  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < kIngestThreads; ++index) {
    threads.emplace_back([&, index]() {
      for (std::size_t round = 0; round < kPerThread; ++round) {
        const Outcome<IngestOutcome> outcome = self.fabric->ingest(
            make_gauge(self.link, sources[index], kRxLevel, -3.0, round + 1,
                       1'700'000'000'000'000'000LL + static_cast<i64>(round) * 1'000'000'000LL,
                       Unit::DecibelMilliwatt));
        if (!outcome.ok()) {
          failures[index] = outcome.status().to_text();
          rejected.fetch_add(1);
          return;
        }
        accepted.fetch_add(1);
      }
    });
  }
  for (std::size_t index = 0; index < kQueryThreads; ++index) {
    threads.emplace_back([&, index]() {
      QualityQuery query;
      query.link = self.link;
      while (!stop_queries.load()) {
        Outcome<LinkQualityReport> report = self.fabric->query(query);
        if (!report.ok()) {
          failures[kIngestThreads + index] = report.status().to_text();
          return;
        }
        // A report never contradicts itself: a healthy state always carries
        // fresh evidence, and an unknown state never carries a value.
        if (report.value().overall == QualityState::Healthy &&
            report.value().evidence_fresh == 0) {
          failures[kIngestThreads + index] = "healthy without fresh evidence";
          return;
        }
        if (report.value().overall == QualityState::Unknown) {
          for (const MetricAssessment& assessment : report.value().metrics) {
            if (assessment.value.has_value()) {
              failures[kIngestThreads + index] = "unknown state carried a value";
              return;
            }
          }
        }
      }
    });
  }
  for (std::size_t index = 0; index < kIngestThreads; ++index) {
    threads[index].join();
  }
  stop_queries.store(true);
  for (std::size_t index = kIngestThreads; index < threads.size(); ++index) {
    threads[index].join();
  }
  for (const std::string& failure : failures) {
    LQF_CHECK_MSG(failure.empty(), failure);
  }
  LQF_CHECK_EQ(accepted.load(), kIngestThreads * kPerThread);
  LQF_CHECK_EQ(rejected.load(), std::size_t{0});

  const FabricStats stats = self.fabric->stats();
  LQF_CHECK_EQ(stats.observations_accepted, static_cast<u64>(kIngestThreads * kPerThread));
  LQF_CHECK_EQ(stats.streams, kIngestThreads);
  LQF_CHECK_EQ(locks::violation_count(), std::size_t{0});
}

LQF_TEST(concurrency, concurrent_publication_and_compaction_stay_consistent) {
  TempDir directory("concurrent-journal");
  const std::string path = directory.file("state.lqf");

  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  auto clock = std::static_pointer_cast<ManualClock>(config.clock);
  config.forced_epoch = FabricEpoch(0xC0FFEEULL);
  config.journal.enabled = true;
  config.journal.path = path;
  config.journal.fsync_on_flush = true;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());

  const LinkIdentity link(LinkId(std::string("link-conc")), LinkGeneration(1));

  constexpr std::size_t kIngestThreads = 3;
  constexpr std::size_t kPerThread = 120;
  // Each thread owns its own source identity: sequence numbers are scoped to a
  // source, so two threads sharing one would collide by design.
  std::vector<SourceIdentity> sources;
  for (std::size_t index = 0; index < kIngestThreads; ++index) {
    const SourceIdentity worker(SourceId(std::string("src-conc-") + text::format_u64(index)),
                                SourceIncarnation(1));
    sources.push_back(worker);
    LQF_CHECK_STATUS_OK(fabric.value()->declare_capability(
        make_capability(link, worker, {kRxLevel, kErrors})));
  }
  std::atomic<std::size_t> accepted{0};
  std::atomic<bool> failed{false};
  std::string failure_detail{};
  std::mutex failure_mutex{};
  const auto report_failure = [&](const std::string& detail) {
    std::lock_guard<std::mutex> guard(failure_mutex);
    if (failure_detail.empty()) {
      failure_detail = detail;
    }
    failed.store(true);
  };
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < kIngestThreads; ++index) {
    threads.emplace_back([&, index]() {
      for (std::size_t round = 0; round < kPerThread && !failed.load(); ++round) {
        const i64 observed =
            1'700'000'000'000'000'000LL + static_cast<i64>(round) * 1'000'000'000LL;
        const SourceIdentity& source = sources[index];
        const Outcome<IngestOutcome> gauge = fabric.value()->ingest(
            make_gauge(link, source, kRxLevel, -3.0, round + 1, observed,
                       Unit::DecibelMilliwatt));
        if (!gauge.ok()) {
          report_failure("gauge ingest: " + gauge.status().to_text());
          return;
        }
        const Outcome<IngestOutcome> counter = fabric.value()->ingest(
            make_counter(link, source, kErrors, static_cast<u64>(index * kPerThread + round),
                         round + 1, observed, Unit::Count));
        if (!counter.ok()) {
          report_failure("counter ingest: " + counter.status().to_text());
          return;
        }
        accepted.fetch_add(2);
      }
    });
  }
  std::thread publisher([&]() {
    for (int generation = 0; generation < 4; ++generation) {
      PolicyDocument document = default_policy_document();
      document.id = PolicyId(std::string("conc"));
      const Outcome<PolicyStamp> published = fabric.value()->publish_policy(document);
      if (!published.ok()) {
        report_failure("publish: " + published.status().to_text());
        return;
      }
      sleep_millis(1);
    }
  });
  std::thread compactor([&]() {
    for (int round = 0; round < 3; ++round) {
      const Status status = fabric.value()->compact();
      if (!status.ok() && status.code() != StatusCode::Refused) {
        report_failure("compact: " + status.to_text());
        return;
      }
      sleep_millis(2);
    }
  });
  for (std::thread& thread : threads) {
    thread.join();
  }
  publisher.join();
  compactor.join();
  {
    std::lock_guard<std::mutex> guard(failure_mutex);
    LQF_CHECK_MSG(!failed.load(), failure_detail);
  }
  LQF_CHECK_EQ(accepted.load(), kIngestThreads * kPerThread * 2);

  LQF_CHECK_STATUS_OK(fabric.value()->flush());
  LQF_CHECK_STATUS_OK(fabric.value()->close());

  // The journal is consistent after concurrent ingestion, publication and
  // compaction: a fresh incarnation recovers every accepted record.
  FabricConfig reopen;
  reopen.clock = std::make_shared<ManualClock>();
  reopen.forced_epoch = FabricEpoch(0xBEEFULL);
  reopen.journal.enabled = true;
  reopen.journal.path = path;
  Outcome<std::shared_ptr<Fabric>> recovered = Fabric::open(reopen);
  LQF_CHECK(recovered.ok());
  LQF_CHECK(!recovered.value()->recovery_report().degraded);
  const FabricStats stats = recovered.value()->stats();
  // Compaction keeps the state that matters and the retained evidence window,
  // which is bounded per stream. Records written after the last snapshot are in
  // the journal as well, so recovery restores at least one full window per
  // stream and never more than everything that was accepted.
  const u64 retained_per_stream =
      std::min<u64>(static_cast<u64>(kPerThread),
                    static_cast<u64>(FabricLimits{}.max_stream_history));
  const u64 minimum_recovered = static_cast<u64>(kIngestThreads) * 2U * retained_per_stream;
  LQF_CHECK_MSG(stats.observations_recovered >= minimum_recovered,
                "recovered=" + text::format_u64(stats.observations_recovered) + " minimum=" +
                    text::format_u64(minimum_recovered));
  LQF_CHECK(stats.observations_recovered <= accepted.load());
  QualityQuery query;
  query.link = link;
  Outcome<LinkQualityReport> report = recovered.value()->query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK(report.value().overall == QualityState::Stale);
  LQF_CHECK_STATUS_OK(recovered.value()->close());
}
