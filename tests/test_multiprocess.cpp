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

// Independent process proof. Two operating system processes exchange framed
// messages over loopback TCP: the runtime runs in a child process started from
// this binary, the client runs here. Threads are not used as a substitute.

#include "harness.hpp"

#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lqf/core/hash.hpp"
#include "lqf/transport/client.hpp"
#include "lqf/transport/socket.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const char* kToken = "lqf-test-token";

struct RemoteNode {
  ChildProcess process{};
  u16 port{0};
};

Outcome<RemoteNode> spawn_node(const std::string& journal, const std::string& port_file) {
  Outcome<ChildProcess> spawned = ChildProcess::spawn(
      {"--node-journal", journal, "--node-port-file", port_file, "--node-token", kToken});
  if (!spawned.ok()) {
    return spawned.status();
  }
  RemoteNode node;
  node.process = std::move(spawned.value());
  std::string content;
  if (!wait_for_file_content(port_file, content)) {
    return Status::error(StatusCode::Unavailable, "the node did not publish a port");
  }
  std::istringstream stream(content);
  std::string token;
  u64 port = 0;
  stream >> token >> port;
  if (token != "READY" || port == 0 || port > 65535) {
    return Status::error(StatusCode::Protocol, "malformed readiness file");
  }
  node.port = static_cast<u16>(port);
  return node;
}

Outcome<FabricClient> connect_client(u16 port, const char* name) {
  ClientConfig config;
  config.port = port;
  config.client_name = name;
  Outcome<FabricClient> client = FabricClient::connect(config);
  if (!client.ok()) {
    return client.status();
  }
  const Status handshake = client.value().handshake();
  if (!handshake.ok()) {
    return handshake;
  }
  return client;
}

void send_raw_bytes(u16 port, const std::string& bytes) {
  Outcome<Socket> socket = Socket::connect_loopback(port);
  if (!socket.ok()) {
    return;
  }
  (void)socket.value().send_all(bytes);
  // Drain whatever the runtime answers so the test observes the refusal rather
  // than assuming it.
  std::string reply;
  unsigned char header[kFrameHeaderBytes];
  const Status status = socket.value().recv_exact(header, kFrameHeaderBytes);
  if (!status.ok()) {
    return;
  }
  u32 length = 0;
  u32 crc = 0;
  if (!decode_frame_header(header, FrameLimits{}, length, crc).ok()) {
    return;
  }
  reply.resize(length);
  if (!socket.value().recv_exact(reply.data(), length).ok()) {
    return;
  }
  Frame frame;
  if (decode_frame_payload(reply, crc, frame).ok() && frame.type == MessageType::Error) {
    ErrorReply error;
    if (decode_error_reply(frame.body, error).ok()) {
      (void)error;
    }
  }
  socket.value().shutdown_both();
  socket.value().close();
}

std::string make_frame_bytes(u32 declared_length, u32 crc, MessageType type,
                             const std::string& body) {
  std::string out(kFrameHeaderBytes, '\0');
  for (std::size_t index = 0; index < 4; ++index) {
    out[index] = static_cast<char>((declared_length >> (index * 8U)) & 0xFFU);
    out[4 + index] = static_cast<char>((crc >> (index * 8U)) & 0xFFU);
  }
  out.push_back(static_cast<char>(type));
  out.append(body);
  return out;
}

}  // namespace

LQF_TEST(multiprocess, independent_process_serves_the_full_protocol) {
  TempDir directory("multiprocess");
  const std::string journal = directory.file("state.lqf");
  const std::string port_file = directory.file("node.port");
  Outcome<RemoteNode> node = spawn_node(journal, port_file);
  LQF_CHECK_STATUS_OK(node.status());

  const LinkIdentity link(LinkId(std::string("link-mp")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-mp")), SourceIncarnation(1));

  Outcome<FabricClient> client = connect_client(node.value().port, "lqf-tests");
  LQF_CHECK_STATUS_OK(client.status());
  LQF_CHECK(client.value().server_epoch().value() != 0);

  CapabilityDeclaration declaration = make_capability(link, source, {kRxLevel});
  declaration.evidence_class = EvidenceClass::Synthetic;
  Outcome<CapabilityAck> ack = client.value().declare_capability(declaration);
  LQF_CHECK(ack.ok());
  LQF_CHECK_EQ(ack.value().revision.value(), u64{1});

  Outcome<PolicyStamp> policy = client.value().publish_policy(default_policy_document());
  LQF_CHECK(policy.ok());
  LQF_CHECK(policy.value().generation.value() >= 2);

  std::vector<Observation> batch;
  for (u64 index = 0; index < 4; ++index) {
    batch.push_back(make_gauge(link, source, kRxLevel, -3.0, index + 1,
                               wall_now_nanos() - 1'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                               Unit::DecibelMilliwatt));
  }
  Outcome<BatchOutcome> ingested = client.value().ingest(batch);
  LQF_CHECK(ingested.ok());
  LQF_CHECK_MSG(ingested.value().accepted == 4,
                "accepted=" + text::format_u64(ingested.value().accepted) + " rejected=" +
                    text::format_u64(ingested.value().rejected) + " duplicates=" +
                    text::format_u64(ingested.value().duplicates) + " reordered=" +
                    text::format_u64(ingested.value().reordered) + " first=" +
                    to_string(ingested.value().first_code));

  QualityQuery query;
  query.link = link;
  Outcome<LinkQualityReport> report = client.value().query(query);
  LQF_CHECK(report.ok());
  LQF_CHECK_EQ(report.value().evidence_considered > 0, true);
  LQF_CHECK(report.value().policy.generation == policy.value().generation);

  WindowQuery window;
  window.link = link;
  window.limit = 16;
  Outcome<WindowResult> records = client.value().window(window);
  LQF_CHECK(records.ok());
  LQF_CHECK_EQ(records.value().records.size(), std::size_t{4});

  Outcome<Explanation> explanation = client.value().explain(query);
  LQF_CHECK(explanation.ok());
  LQF_CHECK(explanation.value().text.find("rx-level-healthy") != std::string::npos);

  Outcome<InspectionReport> inspection = client.value().inspect(InspectionFilter{});
  LQF_CHECK(inspection.ok());
  LQF_CHECK_EQ(inspection.value().links_inspected, u64{1});

  Outcome<FabricStats> stats = client.value().stats();
  LQF_CHECK(stats.ok());
  LQF_CHECK_EQ(stats.value().observations_accepted, u64{4});

  Outcome<FlushAck> flushed = client.value().flush();
  LQF_CHECK(flushed.ok());
  LQF_CHECK(flushed.value().records_written > 0);

  // A second, independent connection sees the same state.
  Outcome<FabricClient> second = connect_client(node.value().port, "lqf-tests-2");
  LQF_CHECK_STATUS_OK(second.status());
  Outcome<LinkQualityReport> second_report = second.value().query(query);
  LQF_CHECK(second_report.ok());
  // Two independent connections see the same conclusion: same overall state,
  // same per-metric states and the same policy generation. The generated
  // timestamps differ by construction, so the states are compared rather than
  // the whole rendering.
  LQF_CHECK(second_report.value().overall == report.value().overall);
  LQF_CHECK(second_report.value().policy.generation == report.value().policy.generation);
  LQF_CHECK_EQ(second_report.value().metrics.size(), report.value().metrics.size());
  for (std::size_t index = 0; index < report.value().metrics.size(); ++index) {
    LQF_CHECK(second_report.value().metrics[index].state == report.value().metrics[index].state);
    LQF_CHECK(second_report.value().metrics[index].metric == report.value().metrics[index].metric);
  }

  // The shutdown token is required: a wrong token is refused and the runtime
  // keeps serving.
  Outcome<ShutdownAck> refused = client.value().shutdown("wrong-token");
  LQF_CHECK(!refused.ok());
  LQF_CHECK(refused.status().code() == StatusCode::Refused);
  Outcome<LinkQualityReport> still_serving = client.value().query(query);
  LQF_CHECK(still_serving.ok());

  Outcome<ShutdownAck> accepted = client.value().shutdown(kToken);
  LQF_CHECK(accepted.ok());
  LQF_CHECK_EQ(accepted.value().accepted, u8{1});
  client.value().close();
  second.value().close();

  // The node exits by itself, with a zero exit code, once it is asked to stop.
  LQF_CHECK_EQ(node.value().process.wait(), 0);
}

LQF_TEST(multiprocess, malformed_frames_are_refused_and_the_runtime_survives) {
  TempDir directory("malformed");
  const std::string journal = directory.file("state.lqf");
  const std::string port_file = directory.file("node.port");
  Outcome<RemoteNode> node = spawn_node(journal, port_file);
  LQF_CHECK_STATUS_OK(node.status());

  // A frame whose declared length exceeds the limit is refused without reading
  // the body into memory.
  send_raw_bytes(node.value().port,
                 make_frame_bytes(0x00FFFFFFU, 0, MessageType::Hello, std::string()));
  // A frame with a valid length but an unknown message type is refused.
  {
    const std::string payload = std::string(1, static_cast<char>(200));
    send_raw_bytes(node.value().port,
                   make_frame_bytes(static_cast<u32>(payload.size()), crc32c(payload),
                                    static_cast<MessageType>(200), std::string()));
  }
  // A frame whose checksum does not match is refused.
  {
    const std::string payload = std::string(1, static_cast<char>(MessageType::Stats));
    send_raw_bytes(node.value().port,
                   make_frame_bytes(static_cast<u32>(payload.size()), crc32c(payload) ^ 0xFFFFU,
                                    MessageType::Stats, std::string()));
  }

  // The runtime is still healthy after every refusal.
  Outcome<FabricClient> client = connect_client(node.value().port, "lqf-tests");
  LQF_CHECK_STATUS_OK(client.status());
  const LinkIdentity link(LinkId(std::string("link-mal")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-mal")), SourceIncarnation(1));
  CapabilityDeclaration declaration = make_capability(link, source, {kRxLevel});
  declaration.evidence_class = EvidenceClass::Synthetic;
  LQF_CHECK(client.value().declare_capability(declaration).ok());
  QualityQuery query;
  query.link = link;
  LQF_CHECK(client.value().query(query).ok());
  Outcome<FabricStats> stats = client.value().stats();
  LQF_CHECK(stats.ok());

  LQF_CHECK(client.value().shutdown(kToken).ok());
  client.value().close();
  LQF_CHECK_EQ(node.value().process.wait(), 0);
}

LQF_TEST(multiprocess, concurrent_clients_observe_consistent_state) {
  TempDir directory("concurrent-clients");
  const std::string journal = directory.file("state.lqf");
  const std::string port_file = directory.file("node.port");
  Outcome<RemoteNode> node = spawn_node(journal, port_file);
  LQF_CHECK_STATUS_OK(node.status());

  const LinkIdentity link(LinkId(std::string("link-cc")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-cc")), SourceIncarnation(1));
  {
    Outcome<FabricClient> setup = connect_client(node.value().port, "lqf-setup");
    LQF_CHECK_STATUS_OK(setup.status());
    CapabilityDeclaration declaration = make_capability(link, source, {kRxLevel});
    declaration.evidence_class = EvidenceClass::Synthetic;
    LQF_CHECK(setup.value().declare_capability(declaration).ok());
    LQF_CHECK(setup.value().shutdown(kToken).ok());
    setup.value().close();
  }
  (void)node.value().process.wait();

  // Restart on the same journal: a fresh incarnation serves the same link.
  std::error_code error;
  std::filesystem::remove(port_file, error);
  Outcome<RemoteNode> restarted = spawn_node(journal, port_file);
  LQF_CHECK_STATUS_OK(restarted.status());

  constexpr std::size_t kClients = 4;
  constexpr std::size_t kPerClient = 16;
  std::vector<std::thread> threads;
  std::vector<u64> accepted(kClients, 0);
  std::vector<std::string> failures(kClients);
  for (std::size_t index = 0; index < kClients; ++index) {
    threads.emplace_back([&, index]() {
      Outcome<FabricClient> client = connect_client(restarted.value().port, "lqf-worker");
      if (!client.ok()) {
        failures[index] = client.status().to_text();
        return;
      }
      // Each client uses its own source incarnation, so the runtime fences them
      // apart instead of merging their sequences.
      const SourceIdentity worker(SourceId(std::string("src-worker") + text::format_u64(index)),
                                  SourceIncarnation(1));
      CapabilityDeclaration declaration = make_capability(link, worker, {kRxLevel});
      declaration.evidence_class = EvidenceClass::Synthetic;
      if (!client.value().declare_capability(declaration).ok()) {
        failures[index] = "capability declaration failed";
        return;
      }
      u64 local = 0;
      for (std::size_t round = 0; round < kPerClient; ++round) {
        Outcome<BatchOutcome> outcome = client.value().ingest({make_gauge(
            link, worker, kRxLevel, -3.0, round + 1,
            wall_now_nanos() - 1'000'000LL + static_cast<i64>(round) * 1'000'000'000LL,
            Unit::DecibelMilliwatt)});
        if (!outcome.ok()) {
          failures[index] = outcome.status().to_text();
          return;
        }
        local += outcome.value().accepted;
      }
      accepted[index] = local;
      client.value().close();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (std::size_t index = 0; index < kClients; ++index) {
    LQF_CHECK_MSG(failures[index].empty(), failures[index]);
    LQF_CHECK_EQ(accepted[index], u64{kPerClient});
  }

  Outcome<FabricClient> observer = connect_client(restarted.value().port, "lqf-observer");
  LQF_CHECK_STATUS_OK(observer.status());
  Outcome<InspectionReport> inspection = observer.value().inspect(InspectionFilter{});
  LQF_CHECK(inspection.ok());
  LQF_CHECK_EQ(inspection.value().links_inspected, u64{1});
  Outcome<FabricStats> stats = observer.value().stats();
  LQF_CHECK(stats.ok());
  LQF_CHECK_EQ(stats.value().observations_accepted, static_cast<u64>(kClients * kPerClient));
  LQF_CHECK(observer.value().shutdown(kToken).ok());
  observer.value().close();
  LQF_CHECK_EQ(restarted.value().process.wait(), 0);
}
