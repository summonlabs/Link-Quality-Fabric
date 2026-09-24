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

// Loopback TCP transport tests. The multiprocess suite proves the same
// transport across two operating system processes; this suite proves the
// framing, the refusal paths and the shutdown paths in one process.

#include "harness.hpp"

#include <string>
#include <vector>

#include "lqf/core/hash.hpp"
#include "lqf/transport/client.hpp"
#include "lqf/transport/server.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");
const char* kToken = "transport-token";

struct Harness {
  std::shared_ptr<Fabric> fabric{};
  std::unique_ptr<FabricServer> server{};
  ~Harness() {
    if (server != nullptr) {
      (void)server->stop();
    }
    if (fabric != nullptr) {
      (void)fabric->close();
    }
  }
};

Outcome<std::unique_ptr<Harness>> start_harness(std::size_t max_connections = 8) {
  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(0x70001ULL);
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  if (!fabric.ok()) {
    return fabric.status();
  }
  auto harness = std::make_unique<Harness>();
  harness->fabric = fabric.value();
  ServerConfig server_config;
  server_config.port = 0;
  server_config.shutdown_token = kToken;
  server_config.max_connections = max_connections;
  harness->server = std::make_unique<FabricServer>(harness->fabric, server_config);
  const Status started = harness->server->start();
  if (!started.ok()) {
    return started;
  }
  return harness;
}

}  // namespace

LQF_TEST(transport, request_reply_over_real_loopback) {
  Outcome<std::unique_ptr<Harness>> started = start_harness();
  LQF_CHECK_STATUS_OK(started.status());
  Harness& harness = *started.value();

  ClientConfig config;
  config.port = harness.server->port();
  config.client_name = "transport-tests";
  Outcome<FabricClient> client = FabricClient::connect(config);
  LQF_CHECK_STATUS_OK(client.status());
  LQF_CHECK_STATUS_OK(client.value().handshake());
  LQF_CHECK_EQ(client.value().server_epoch().value(), u64{0x70001ULL});

  const LinkIdentity link(LinkId(std::string("link-t")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-t")), SourceIncarnation(1));
  CapabilityDeclaration declaration = make_capability(link, source, {kRxLevel});
  declaration.evidence_class = EvidenceClass::Synthetic;
  LQF_CHECK(client.value().declare_capability(declaration).ok());

  std::vector<Observation> batch;
  for (u64 index = 0; index < 3; ++index) {
    batch.push_back(make_gauge(link, source, kRxLevel, -3.0, index + 1,
                               1'700'000'000'000'000'000LL + static_cast<i64>(index) * 1'000'000'000LL,
                               Unit::DecibelMilliwatt));
  }
  Outcome<BatchOutcome> ingested = client.value().ingest(batch);
  LQF_CHECK(ingested.ok());
  LQF_CHECK_EQ(ingested.value().accepted, u64{3});

  QualityQuery query;
  query.link = link;
  Outcome<LinkQualityReport> report = client.value().query(query);
  LQF_CHECK(report.ok());
  // Classification considers the newest reading per stream, so exactly one
  // piece of evidence supports the conclusion; all three are still retained and
  // visible in the historical window.
  LQF_CHECK_EQ(report.value().evidence_considered, u64{1});
  LQF_CHECK_EQ(report.value().evidence_fresh, u64{1});

  WindowQuery window;
  window.link = link;
  window.limit = 8;
  LQF_CHECK(client.value().window(window).ok());

  // The declared capability view and the published policy document cross the
  // wire with their provenance, not as an inference from evidence.
  Outcome<std::vector<MetricCapabilityView>> views = client.value().capabilities(link);
  LQF_CHECK(views.ok());
  LQF_CHECK_EQ(views.value().size(), std::size_t{1});
  LQF_CHECK_EQ(views.value()[0].declared_by.size(), std::size_t{1});
  LQF_CHECK(views.value()[0].declared_by[0].unit == Unit::DecibelMilliwatt);
  LQF_CHECK_EQ(views.value()[0].declared_by[0].evidence_class, EvidenceClass::Synthetic);

  const PolicyStamp published = client.value().stats().value().policy;
  Outcome<PolicyGenerationRecord> document = client.value().policy_document(PolicyGeneration(0));
  LQF_CHECK(document.ok());
  LQF_CHECK(document.value().stamp.generation == published.generation);
  LQF_CHECK_EQ(canonical_policy_text(document.value().document),
               canonical_policy_text(default_policy_document()));
  // A generation that is not retained is reported as not found, never as an
  // empty document.
  LQF_CHECK(!client.value().policy_document(PolicyGeneration(9999)).ok());
  LQF_CHECK(client.value().explain(query).ok());
  LQF_CHECK(client.value().inspect(InspectionFilter{}).ok());
  LQF_CHECK(client.value().stats().ok());
  LQF_CHECK(client.value().flush().status().code() == StatusCode::Refused);
  LQF_CHECK(client.value().compact().status().code() == StatusCode::Refused);

  const ServerStats stats = harness.server->stats();
  LQF_CHECK_EQ(stats.connections_accepted, u64{1});
  LQF_CHECK(stats.frames_in >= 8);
  LQF_CHECK(stats.frames_out >= 8);
  LQF_CHECK_EQ(stats.protocol_errors, u64{0});

  LQF_CHECK(client.value().shutdown(kToken).ok());
  client.value().close();
  harness.server->wait();
  LQF_CHECK_STATUS_OK(harness.server->stop());
}

LQF_TEST(transport, protocol_errors_are_answered_and_survivable) {
  Outcome<std::unique_ptr<Harness>> started = start_harness();
  LQF_CHECK_STATUS_OK(started.status());
  Harness& harness = *started.value();

  // A peer that sends an unknown message type gets an explicit refusal and the
  // connection is closed.
  {
    Outcome<Socket> socket = Socket::connect_loopback(harness.server->port());
    LQF_CHECK_STATUS_OK(socket.status());
    const std::string unknown = std::string(1, static_cast<char>(240));
    std::string frame(kFrameHeaderBytes, '\0');
    const u32 length = static_cast<u32>(unknown.size());
    const u32 crc = crc32c(unknown);
    for (std::size_t index = 0; index < 4; ++index) {
      frame[index] = static_cast<char>((length >> (index * 8U)) & 0xFFU);
      frame[4 + index] = static_cast<char>((crc >> (index * 8U)) & 0xFFU);
    }
    frame.append(unknown);
    LQF_CHECK_STATUS_OK(socket.value().send_all(frame));
    unsigned char header[kFrameHeaderBytes];
    const Status read = socket.value().recv_exact(header, kFrameHeaderBytes);
    LQF_CHECK(read.ok());
    u32 reply_length = 0;
    u32 reply_crc = 0;
    LQF_CHECK_STATUS_OK(decode_frame_header(header, FrameLimits{}, reply_length, reply_crc));
    std::string payload(reply_length, '\0');
    LQF_CHECK_STATUS_OK(socket.value().recv_exact(payload.data(), reply_length));
    Frame reply;
    LQF_CHECK_STATUS_OK(decode_frame_payload(payload, reply_crc, reply));
    LQF_CHECK(reply.type == MessageType::Error);
    ErrorReply error;
    LQF_CHECK_STATUS_OK(decode_error_reply(reply.body, error));
    LQF_CHECK(error.code == StatusCode::Protocol);
    socket.value().shutdown_both();
    socket.value().close();
  }

  // A peer that sends a length beyond the frame limit is refused without the
  // runtime allocating anything.
  {
    Outcome<Socket> socket = Socket::connect_loopback(harness.server->port());
    LQF_CHECK_STATUS_OK(socket.status());
    std::string frame(kFrameHeaderBytes, '\0');
    const u32 length = 0x00FFFFFFU;
    for (std::size_t index = 0; index < 4; ++index) {
      frame[index] = static_cast<char>((length >> (index * 8U)) & 0xFFU);
    }
    LQF_CHECK_STATUS_OK(socket.value().send_all(frame));
    unsigned char header[kFrameHeaderBytes];
    LQF_CHECK(socket.value().recv_exact(header, kFrameHeaderBytes).ok());
    u32 reply_length = 0;
    u32 reply_crc = 0;
    LQF_CHECK_STATUS_OK(decode_frame_header(header, FrameLimits{}, reply_length, reply_crc));
    std::string payload(reply_length, '\0');
    LQF_CHECK(socket.value().recv_exact(payload.data(), reply_length).ok());
    Frame reply;
    LQF_CHECK_STATUS_OK(decode_frame_payload(payload, reply_crc, reply));
    LQF_CHECK(reply.type == MessageType::Error);
    socket.value().shutdown_both();
    socket.value().close();
  }

  // A peer whose frame checksum does not match is refused.
  {
    Outcome<Socket> socket = Socket::connect_loopback(harness.server->port());
    LQF_CHECK_STATUS_OK(socket.status());
    const std::string payload = std::string(1, static_cast<char>(MessageType::Stats));
    std::string frame(kFrameHeaderBytes, '\0');
    const u32 length = static_cast<u32>(payload.size());
    const u32 crc = crc32c(payload) ^ 0x5A5AU;
    for (std::size_t index = 0; index < 4; ++index) {
      frame[index] = static_cast<char>((length >> (index * 8U)) & 0xFFU);
      frame[4 + index] = static_cast<char>((crc >> (index * 8U)) & 0xFFU);
    }
    frame.append(payload);
    LQF_CHECK_STATUS_OK(socket.value().send_all(frame));
    unsigned char reply_header[kFrameHeaderBytes];
    LQF_CHECK(socket.value().recv_exact(reply_header, kFrameHeaderBytes).ok());
    socket.value().shutdown_both();
    socket.value().close();
  }

  // A peer that connects and disappears mid message does not disturb the
  // runtime.
  {
    Outcome<Socket> socket = Socket::connect_loopback(harness.server->port());
    LQF_CHECK_STATUS_OK(socket.status());
    LQF_CHECK_STATUS_OK(socket.value().send_all(std::string(3, 'x')));
    socket.value().shutdown_both();
    socket.value().close();
  }

  // The runtime still serves a well behaved client.
  sleep_millis(5);
  ClientConfig config;
  config.port = harness.server->port();
  Outcome<FabricClient> client = FabricClient::connect(config);
  LQF_CHECK_STATUS_OK(client.status());
  LQF_CHECK_STATUS_OK(client.value().handshake());
  LQF_CHECK(client.value().stats().ok());
  LQF_CHECK(client.value().shutdown(kToken).ok());
  client.value().close();
  harness.server->wait();
  LQF_CHECK_STATUS_OK(harness.server->stop());
  LQF_CHECK(harness.server->stats().protocol_errors >= 3);
}

LQF_TEST(transport, wakeup_channel_interrupts_a_blocked_wait) {
  Outcome<WakeupPair> pair = WakeupPair::create();
  LQF_CHECK_STATUS_OK(pair.status());
  bool socket_ready = false;
  bool wake_ready = false;
  // A wait with no wake signal would block forever: the wakeup channel is the
  // only reason this returns, and it is a real signal rather than a timeout.
  LQF_CHECK_STATUS_OK(pair.value().wake());
  LQF_CHECK_STATUS_OK(Socket::wait_readable(lqf_invalid_socket, pair.value().read_socket(),
                                            socket_ready, wake_ready));
  LQF_CHECK(wake_ready);
  LQF_CHECK(!socket_ready);
  LQF_CHECK_STATUS_OK(pair.value().drain());
  pair.value().close();
}

LQF_TEST(transport, unconnected_client_is_refused_cleanly) {
  // A port of zero is a configuration error and is refused before any socket is
  // created.
  const Outcome<FabricClient> invalid = FabricClient::connect(ClientConfig{});
  LQF_CHECK(!invalid.ok());
  LQF_CHECK(invalid.status().code() == StatusCode::Invalid);

  // Nothing listens on this port: the connection attempt fails with an explicit
  // status instead of a hang or a silent success.
  ClientConfig config;
  config.port = 1;
  const Outcome<FabricClient> refused = FabricClient::connect(config);
  LQF_CHECK(!refused.ok());
  LQF_CHECK(!refused.status().message().empty());
}
