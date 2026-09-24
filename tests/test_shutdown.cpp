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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "lqf/transport/client.hpp"
#include "lqf/transport/server.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const char* kToken = "shutdown-token";

}  // namespace

LQF_TEST(shutdown, close_is_idempotent_and_refuses_new_work) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));
  LQF_CHECK_STATUS_OK(self.fabric->close());
  LQF_CHECK_STATUS_OK(self.fabric->close());
  LQF_CHECK(!self.fabric->is_open());
  const Outcome<IngestOutcome> after_close = self.fabric->ingest(
      make_gauge(self.link, self.source, kRxLevel, -3.0, 2, self.clock->now().wall_nanos,
                 Unit::DecibelMilliwatt));
  LQF_CHECK(!after_close.ok());
  LQF_CHECK(after_close.status().code() == StatusCode::Unavailable);
  // Queries keep working on the frozen state rather than crashing.
  QualityQuery query;
  query.link = self.link;
  LQF_CHECK(self.fabric->query(query).ok());
}

LQF_TEST(shutdown, ingestion_accepted_before_close_is_durable) {
  TempDir directory("durable");
  const std::string path = directory.file("state.lqf");
  const LinkIdentity link(LinkId(std::string("link-d")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-d")), SourceIncarnation(1));

  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(0xDEADBEEFULL);
  config.journal.enabled = true;
  config.journal.path = path;
  config.journal.fsync_on_flush = true;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());
  LQF_CHECK_STATUS_OK(
      fabric.value()->declare_capability(make_capability(link, source, {kRxLevel})));

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> refused{0};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < 2; ++index) {
    threads.emplace_back([&, index]() {
      u64 sequence = 1;
      while (!stop.load()) {
        const Outcome<IngestOutcome> outcome = fabric.value()->ingest(
            make_gauge(link, source, kRxLevel, -3.0, sequence++,
                       1'700'000'000'000'000'000LL + static_cast<i64>(sequence) * 1'000'000LL,
                       Unit::DecibelMilliwatt));
        if (outcome.ok() && outcome.value().accepted) {
          accepted.fetch_add(1);
        } else if (!outcome.ok()) {
          refused.fetch_add(1);
        }
        if (accepted.load() > 400) {
          break;
        }
      }
    });
  }
  while (accepted.load() < 400) {
    sleep_millis(1);
  }
  stop.store(true);
  // Close while both threads are still inside the ingest path: no lock is held
  // by the closer that a worker needs, and every accepted record was journaled
  // before its acceptance was reported.
  LQF_CHECK_STATUS_OK(fabric.value()->close());
  for (std::thread& thread : threads) {
    thread.join();
  }
  const u64 accepted_count = accepted.load();
  LQF_CHECK(accepted_count > 0);
  LQF_CHECK_EQ(locks::violation_count(), std::size_t{0});

  FabricConfig reopen;
  reopen.clock = std::make_shared<ManualClock>();
  reopen.forced_epoch = FabricEpoch(0xFEEDULL);
  reopen.journal.enabled = true;
  reopen.journal.path = path;
  Outcome<std::shared_ptr<Fabric>> recovered = Fabric::open(reopen);
  LQF_CHECK(recovered.ok());
  LQF_CHECK(!recovered.value()->recovery_report().degraded);
  // Everything that was reported accepted is present after recovery.
  LQF_CHECK(recovered.value()->stats().observations_recovered >= accepted_count);
  LQF_CHECK_STATUS_OK(recovered.value()->close());
}

LQF_TEST(shutdown, server_stop_joins_every_thread_and_is_idempotent) {
  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(0x5150ULL);
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());

  ServerConfig server_config;
  server_config.port = 0;
  server_config.shutdown_token = kToken;
  FabricServer server(fabric.value(), server_config);
  LQF_CHECK_STATUS_OK(server.start());
  LQF_CHECK(server.port() != 0);
  LQF_CHECK(server.running());

  // Two live connections that are deliberately left idle: stop must unblock
  // them rather than wait for them.
  ClientConfig client_config;
  client_config.port = server.port();
  Outcome<FabricClient> first = FabricClient::connect(client_config);
  LQF_CHECK_STATUS_OK(first.status());
  LQF_CHECK_STATUS_OK(first.value().handshake());
  Outcome<FabricClient> second = FabricClient::connect(client_config);
  LQF_CHECK_STATUS_OK(second.status());
  LQF_CHECK_STATUS_OK(second.value().handshake());

  const ServerStats before = server.stats();
  LQF_CHECK_EQ(before.connections_accepted, u64{2});
  LQF_CHECK_EQ(before.active_connections, std::size_t{2});

  // The stop path is a real signal: it must return promptly even with two idle
  // connections attached, and both threads must be joined by the time it does.
  LQF_CHECK_STATUS_OK(server.stop());
  LQF_CHECK(!server.running());
  LQF_CHECK_STATUS_OK(server.stop());
  const ServerStats after = server.stats();
  LQF_CHECK_EQ(after.active_connections, std::size_t{0});
  LQF_CHECK(!after.running);

  // The idle clients observe the closed connection instead of hanging.
  QualityQuery query;
  query.link = LinkIdentity(LinkId(std::string("link-s")), LinkGeneration(1));
  const Outcome<LinkQualityReport> report = first.value().query(query);
  LQF_CHECK(!report.ok());
  first.value().close();
  second.value().close();
  LQF_CHECK_STATUS_OK(fabric.value()->close());
}

LQF_TEST(shutdown, server_refuses_new_connections_beyond_the_limit) {
  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(0x7777ULL);
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());

  ServerConfig server_config;
  server_config.port = 0;
  server_config.shutdown_token = kToken;
  server_config.max_connections = 2;
  FabricServer server(fabric.value(), server_config);
  LQF_CHECK_STATUS_OK(server.start());

  ClientConfig client_config;
  client_config.port = server.port();
  std::vector<FabricClient> clients;
  for (int index = 0; index < 2; ++index) {
    Outcome<FabricClient> client = FabricClient::connect(client_config);
    LQF_CHECK_STATUS_OK(client.status());
    LQF_CHECK_STATUS_OK(client.value().handshake());
    clients.push_back(std::move(client.value()));
  }
  // Beyond the limit a connection is either served (if a slot has already been
  // released) or refused with an explicit error. It is never silently dropped
  // and the server never exceeds its own bound.
  Outcome<FabricClient> third = FabricClient::connect(client_config);
  if (third.ok()) {
    const Status handshake = third.value().handshake();
    if (!handshake.ok()) {
      LQF_CHECK(handshake.code() == StatusCode::Busy ||
                handshake.code() == StatusCode::Cancelled);
    }
    third.value().close();
  }
  const ServerStats stats = server.stats();
  LQF_CHECK_EQ(stats.connections_accepted, u64{2});
  LQF_CHECK(stats.connections_rejected <= 1);
  LQF_CHECK(stats.active_connections <= 2);

  for (FabricClient& client : clients) {
    client.close();
  }
  LQF_CHECK_STATUS_OK(server.stop());
  LQF_CHECK_STATUS_OK(fabric.value()->close());
}
