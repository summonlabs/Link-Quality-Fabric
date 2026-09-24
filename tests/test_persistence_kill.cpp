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

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lqf/transport/client.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const char* kToken = "lqf-test-token";

struct Node {
  ChildProcess process{};
  u16 port{0};
  std::shared_ptr<FabricClient> client{};
};

Outcome<Node> start_node(const std::string& journal, const std::string& port_file) {
  Outcome<ChildProcess> spawned = ChildProcess::spawn(
      {"--node-journal", journal, "--node-port-file", port_file, "--node-token", kToken});
  if (!spawned.ok()) {
    return spawned.status();
  }
  Node node;
  node.process = std::move(spawned.value());
  std::string content;
  if (!wait_for_file_content(port_file, content)) {
    return Status::error(StatusCode::Unavailable,
                         "the child node never published a listening port");
  }
  std::istringstream stream(content);
  std::string token;
  u64 port = 0;
  stream >> token >> port;
  if (token != "READY" || port == 0 || port > 65535) {
    return Status::error(StatusCode::Protocol, "the readiness file was malformed: " + content);
  }
  node.port = static_cast<u16>(port);
  ClientConfig config;
  config.port = node.port;
  config.client_name = "lqf-tests";
  Outcome<FabricClient> client = FabricClient::connect(config);
  if (!client.ok()) {
    return client.status();
  }
  node.client = std::make_shared<FabricClient>(std::move(client.value()));
  const Status handshake = node.client->handshake();
  if (!handshake.ok()) {
    return handshake;
  }
  return node;
}

CapabilityDeclaration node_capability(const LinkIdentity& link, const SourceIdentity& source) {
  CapabilityDeclaration declaration = make_capability(link, source, {kRxLevel});
  declaration.evidence_class = EvidenceClass::Synthetic;
  return declaration;
}

}  // namespace

LQF_TEST(persistence_kill, killed_node_restarts_with_recovered_but_stale_evidence) {
  TempDir directory("kill");
  const std::string journal = directory.file("state.lqf");
  const std::string port_file = directory.file("node.port");
  const LinkIdentity link(LinkId(std::string("link-kill")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-kill")), SourceIncarnation(1));

  u64 first_epoch = 0;
  u64 accepted = 0;
  Observation first_observation{};
  {
    Outcome<Node> node = start_node(journal, port_file);
    LQF_CHECK_STATUS_OK(node.status());
    first_epoch = node.value().client->server_epoch().value();
    LQF_CHECK(first_epoch != 0);
    LQF_CHECK_STATUS_OK(node.value().client->declare_capability(node_capability(link, source)));
    std::vector<Observation> batch;
    const i64 observed_base = wall_now_nanos() - 1'000'000LL;
    for (u64 index = 0; index < 8; ++index) {
      batch.push_back(make_gauge(link, source, kRxLevel, -3.0, index + 1,
                                 observed_base + static_cast<i64>(index) * 1'000'000'000LL,
                                 Unit::DecibelMilliwatt));
    }
    first_observation = batch.front();
    Outcome<BatchOutcome> ingested = node.value().client->ingest(batch);
    LQF_CHECK(ingested.ok());
    accepted = ingested.value().accepted;
    LQF_CHECK_EQ(accepted, u64{8});
    Outcome<FlushAck> flushed = node.value().client->flush();
    LQF_CHECK(flushed.ok());

    // The node is killed outright at this lifecycle boundary: no graceful
    // shutdown, no destructor, no flush.
    node.value().process.terminate();
    const int exit_code = node.value().process.wait();
    LQF_CHECK(exit_code != 0);
    LQF_CHECK(!node.value().process.running());
  }

  // A second, independent process opens the same journal.
  std::error_code error;
  std::filesystem::remove(port_file, error);
  Outcome<Node> restarted = start_node(journal, port_file);
  LQF_CHECK_STATUS_OK(restarted.status());
  const u64 second_epoch = restarted.value().client->server_epoch().value();
  LQF_CHECK(second_epoch != first_epoch);

  Outcome<FabricStats> stats = restarted.value().client->stats();
  LQF_CHECK(stats.ok());
  LQF_CHECK(stats.value().recovery.records_read > 0);
  LQF_CHECK(!stats.value().recovery.degraded);
  LQF_CHECK_EQ(stats.value().observations_recovered, accepted);

  QualityQuery query;
  query.link = link;
  Outcome<LinkQualityReport> report = restarted.value().client->query(query);
  LQF_CHECK(report.ok());
  // Fenced by incarnation: recovered evidence never becomes fresh, whatever the
  // wall clock says.
  LQF_CHECK(report.value().overall == QualityState::Stale);
  // The conclusion rests on the newest recovered reading; every recovered record
  // is still retained and reported as recovered.
  LQF_CHECK_EQ(report.value().evidence_recovered, u64{1});
  LQF_CHECK_EQ(report.value().epoch.value(), second_epoch);
  WindowQuery recovered_window;
  recovered_window.link = link;
  recovered_window.limit = 64;
  Outcome<WindowResult> retained = restarted.value().client->window(recovered_window);
  LQF_CHECK(retained.ok());
  LQF_CHECK_EQ(retained.value().records.size(), accepted);
  for (const EvidenceRecord& record : retained.value().records) {
    LQF_CHECK(record.origin == EvidenceOrigin::Recovered);
  }

  // The capability declaration survived the restart: evidence for the same
  // metric is still accepted from the same source without redeclaring it.
  Outcome<BatchOutcome> after_restart = restarted.value().client->ingest(
      {make_gauge(link, source, kRxLevel, -3.0, 9, wall_now_nanos() - 1'000'000LL,
                  Unit::DecibelMilliwatt)});
  LQF_CHECK(after_restart.ok());
  LQF_CHECK_EQ(after_restart.value().accepted, u64{1});

  // A frame from the previous incarnation is refused rather than replayed. The
  // identical frame is the strongest form of the test: it must be recognised as
  // a duplicate even though this is a different process.
  Outcome<BatchOutcome> replayed = restarted.value().client->ingest({first_observation});
  LQF_CHECK(replayed.ok());
  LQF_CHECK_EQ(replayed.value().duplicates, u64{1});
  LQF_CHECK_EQ(replayed.value().accepted, u64{0});
  // The same identity with different content is an identity mismatch, never a
  // silent overwrite.
  Observation tampered = first_observation;
  std::get<GaugeReading>(tampered.reading).value = -9.0;
  const Outcome<BatchOutcome> mismatch = restarted.value().client->ingest({tampered});
  LQF_CHECK(mismatch.ok());
  LQF_CHECK_EQ(mismatch.value().rejected, u64{1});
  LQF_CHECK(mismatch.value().first_code == StatusCode::IdMismatch);

  // A graceful shutdown through the protocol stops the node with exit code 0.
  Outcome<ShutdownAck> shutdown = restarted.value().client->shutdown(kToken);
  LQF_CHECK(shutdown.ok());
  LQF_CHECK_EQ(shutdown.value().accepted, u8{1});
  restarted.value().client->close();
  LQF_CHECK_EQ(restarted.value().process.wait(), 0);
}

LQF_TEST(persistence_kill, crash_between_records_leaves_a_repairable_journal) {
  TempDir directory("kill-torn");
  const std::string journal = directory.file("state.lqf");
  const std::string port_file = directory.file("node.port");
  const LinkIdentity link(LinkId(std::string("link-torn")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-torn")), SourceIncarnation(1));

  {
    Outcome<Node> node = start_node(journal, port_file);
    LQF_CHECK_STATUS_OK(node.status());
    LQF_CHECK_STATUS_OK(node.value().client->declare_capability(node_capability(link, source)));
    for (u64 index = 0; index < 4; ++index) {
      LQF_CHECK(node.value()
                    .client->ingest({make_gauge(link, source, kRxLevel, -3.0, index + 1,
                                                wall_now_nanos() - 1'000'000LL +
                                                    static_cast<i64>(index) * 1'000'000'000LL,
                                                Unit::DecibelMilliwatt)})
                    .ok());
    }
    LQF_CHECK(node.value().client->flush().ok());
    // Kill without a flush of the final bytes: the journal may end mid record.
    node.value().process.terminate();
    (void)node.value().process.wait();
  }

  // Simulate the classic crash residue: a partial record appended to the file.
  {
    std::ifstream stream(journal, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    content.append("PARTIAL");
    std::ofstream out(journal, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  std::error_code error;
  std::filesystem::remove(port_file, error);
  Outcome<Node> restarted = start_node(journal, port_file);
  LQF_CHECK_STATUS_OK(restarted.status());
  Outcome<FabricStats> stats = restarted.value().client->stats();
  LQF_CHECK(stats.ok());
  LQF_CHECK(stats.value().recovery.torn_tail);
  LQF_CHECK(stats.value().recovery.truncated);
  LQF_CHECK(stats.value().recovery.records_read > 0);
  LQF_CHECK(restarted.value().client->shutdown(kToken).ok());
  restarted.value().client->close();
  LQF_CHECK_EQ(restarted.value().process.wait(), 0);
}
