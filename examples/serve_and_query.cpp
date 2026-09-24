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

// Serve and query: a real loopback FabricServer started in a background thread
// on an ephemeral port, a FabricClient that declares a capability, ingests
// evidence and asks for the quality report, then an orderly stop.

#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lqf/core/clock.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/runtime/fabric.hpp"
#include "lqf/transport/client.hpp"
#include "lqf/transport/server.hpp"

namespace {

const lqf::MetricId kRxLevel{lqf::MetricFamily::SignalPower, "rx.level"};

lqf::Observation gauge_observation(const lqf::LinkIdentity& link, const lqf::SourceIdentity& source,
                                   lqf::u64 sequence, double value) {
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
  // An unsynchronized stamp from this host: useful for the record, never
  // treated as comparable across sources.
  observation.observed_at =
      lqf::Timestamp{lqf::make_system_clock()->now().wall_nanos, false};
  observation.provenance.transport = lqf::TransportKind::Synthetic;
  observation.provenance.evidence_class = lqf::EvidenceClass::Synthetic;
  observation.provenance.origin = "serve-and-query/generator";
  observation.provenance.producer = "lqf-example-serve-and-query";
  return observation;
}

}  // namespace

int main() {
  lqf::Outcome<std::shared_ptr<lqf::Fabric>> opened = lqf::Fabric::open(lqf::FabricConfig{});
  if (!opened.ok()) {
    std::cerr << "serve-and-query: the fabric could not be opened: " << opened.status().to_text()
              << "\n";
    return 1;
  }

  lqf::ServerConfig server_config;
  server_config.bind_address = "127.0.0.1";
  server_config.port = 0;  // the loopback stack picks a free port

  lqf::FabricServer server(opened.value(), server_config);
  std::promise<lqf::Status> started;
  std::future<lqf::Status> start_result = started.get_future();
  std::thread server_thread([&server, &started]() { started.set_value(server.start()); });

  // The future is the synchronisation: start() returns only after the listener
  // is bound and the port is published, so the wait below observes a non-zero
  // port instead of guessing at one.
  const lqf::Status start_status = start_result.get();
  if (!start_status.ok()) {
    std::cerr << "serve-and-query: the server did not start: " << start_status.to_text() << "\n";
    server_thread.join();
    return 1;
  }
  lqf::u16 port = 0;
  while (port == 0) {
    port = server.port();
    if (port == 0) {
      std::this_thread::yield();
    }
  }
  std::cout << "server listening " << port << "\n";

  const lqf::LinkIdentity link{lqf::LinkId(std::string("serve-and-query-link")),
                               lqf::LinkGeneration(1)};
  const lqf::SourceIdentity source{lqf::SourceId(std::string("serve-and-query-source")),
                                   lqf::SourceIncarnation(1)};

  lqf::ClientConfig client_config;
  client_config.port = port;
  client_config.client_name = "lqf-example-serve-and-query";
  lqf::Outcome<lqf::FabricClient> connected = lqf::FabricClient::connect(client_config);
  if (!connected.ok()) {
    std::cerr << "serve-and-query: the client could not connect: " << connected.status().to_text()
              << "\n";
    server.stop();
    server_thread.join();
    return 1;
  }
  lqf::FabricClient& client = connected.value();

  const auto fail = [&server, &server_thread, &client](const lqf::Status& status, const char* what) {
    std::cerr << "serve-and-query: " << what << ": " << status.to_text() << "\n";
    client.close();
    server.stop();
    server_thread.join();
    return 1;
  };

  const lqf::Status handshake = client.handshake();
  if (!handshake.ok()) {
    return fail(handshake, "the handshake failed");
  }

  lqf::CapabilityDeclaration declaration;
  declaration.source = source;
  declaration.link_scope = link;
  declaration.revision = lqf::CapabilityRevision(1);
  declaration.transport = lqf::TransportKind::Synthetic;
  declaration.evidence_class = lqf::EvidenceClass::Synthetic;
  declaration.declared_at = lqf::Timestamp{0, false};
  lqf::MetricCapability capability;
  capability.metric = kRxLevel;
  capability.unit = lqf::Unit::DecibelMilliwatt;
  capability.semantics = lqf::SampleSemantics::Gauge;
  capability.lanes_declared = false;
  capability.lane_count = 0;
  declaration.metrics.push_back(capability);

  const lqf::Outcome<lqf::CapabilityAck> declared = client.declare_capability(declaration);
  if (!declared.ok()) {
    return fail(declared.status(), "the capability was refused");
  }

  std::vector<lqf::Observation> observations;
  observations.push_back(gauge_observation(link, source, 1, -6.0));
  observations.push_back(gauge_observation(link, source, 2, -7.5));
  observations.push_back(gauge_observation(link, source, 3, -9.0));
  const lqf::Outcome<lqf::BatchOutcome> ingested = client.ingest(observations);
  if (!ingested.ok()) {
    return fail(ingested.status(), "the batch was refused");
  }
  if (ingested.value().rejected != 0) {
    std::cerr << "serve-and-query: the runtime rejected " << ingested.value().rejected
              << " of the ingested observations\n";
    client.close();
    server.stop();
    server_thread.join();
    return 1;
  }
  std::cout << "ingested accepted=" << ingested.value().accepted
            << " duplicates=" << ingested.value().duplicates
            << " rejected=" << ingested.value().rejected << "\n";

  lqf::QualityQuery query;
  query.link = link;
  query.include_evidence = true;
  const lqf::Outcome<lqf::LinkQualityReport> queried = client.query(query);
  if (!queried.ok()) {
    return fail(queried.status(), "the query failed");
  }
  std::cout << lqf::render_report(queried.value());

  if (!queried.value().link_registered) {
    std::cerr << "serve-and-query: the link is not registered on the server\n";
    client.close();
    server.stop();
    server_thread.join();
    return 1;
  }
  if (!queried.value().evidence_present) {
    std::cerr << "serve-and-query: the server reported no live evidence\n";
    client.close();
    server.stop();
    server_thread.join();
    return 1;
  }

  client.close();
  const lqf::Status stopped = server.stop();
  if (!stopped.ok()) {
    std::cerr << "serve-and-query: the server did not stop cleanly: " << stopped.to_text() << "\n";
    server_thread.join();
    return 1;
  }
  server_thread.join();
  std::cout << "server stopped; the example completed cleanly\n";
  return 0;
}
