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

// lqf emit -- connect to a running runtime, declare a capability for the
// built-in signal-power:rx.level metric and ingest generated gauge readings.
//
// Everything this command produces is generated data. Every observation it
// sends declares EvidenceClass::Synthetic, TransportKind::Synthetic and an
// explicit origin/producer, so a synthetic reading can never be mistaken for a
// measurement of real hardware anywhere downstream.

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cmd_common.hpp"
#include "lqf/core/text.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/transport/client.hpp"

namespace lqf::cli {
namespace {

constexpr u64 kMaxRevisionAttempts = 16;

CommandSpec emit_spec() {
  CommandSpec spec;
  spec.name = "emit";
  spec.summary = "declare a synthetic capability and ingest generated gauge readings";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve (required)"},
      {"--link", true, "<id>", "link identity, optionally <id>/gen<n> (required)"},
      {"--source", true, "<id>", "source identity, optionally <id>@<incarnation> (required)"},
      {"--count", true, "<n>", "number of observations to ingest (default 1)"},
      {"--interval-ms", true, "<n>", "pause between observations in milliseconds (default 0)"},
      {"--synthetic", false, "",
       "acknowledge that the generated evidence is synthetic (it always is)"},
  };
  spec.required = {"--port", "--link", "--source"};
  return spec;
}

// Deterministic, obviously generated levels in dBm. They are fixed constants:
// this command never pretends to have measured anything.
double synthetic_level(u64 index) {
  static constexpr double kPattern[] = {-6.0, -6.75, -7.5, -8.25, -9.0};
  constexpr std::size_t kPatternSize = sizeof(kPattern) / sizeof(kPattern[0]);
  return kPattern[static_cast<std::size_t>(index % static_cast<u64>(kPatternSize))];
}

}  // namespace

int run_emit(const std::vector<std::string>& argv) {
  const CommandSpec spec = emit_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }

  Outcome<u16> port = parse_port_value(*parsed.args.find("--port"), false);
  if (!port.ok()) {
    return usage_error(spec, port.status().message());
  }
  Outcome<LinkIdentity> link = parse_link_value(*parsed.args.find("--link"));
  if (!link.ok()) {
    return usage_error(spec, link.status().message());
  }
  Outcome<SourceIdentity> source = parse_source_value(*parsed.args.find("--source"));
  if (!source.ok()) {
    return usage_error(spec, source.status().message());
  }
  u64 count = 1;
  if (const std::string* text = parsed.args.find("--count")) {
    Outcome<u64> value = parse_u64_value(*text, "count");
    if (!value.ok()) {
      return usage_error(spec, value.status().message());
    }
    if (value.value() == 0) {
      return usage_error(spec, "--count must be at least 1");
    }
    count = value.value();
  }
  u64 interval_millis = 0;
  if (const std::string* text = parsed.args.find("--interval-ms")) {
    Outcome<u64> value = parse_u64_value(*text, "interval");
    if (!value.ok()) {
      return usage_error(spec, value.status().message());
    }
    interval_millis = value.value();
  }

  // The descriptor is looked up in the same built-in catalog the runtime uses,
  // so the declared unit and semantics cannot drift from the metric identity.
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  const MetricDescriptor* descriptor =
      catalog.find(MetricId(MetricFamily::SignalPower, "rx.level"));
  if (descriptor == nullptr) {
    return report_failure(StatusCode::Internal,
                          "the built-in metric signal-power:rx.level is not in the catalog");
  }

  ClientConfig client_config;
  client_config.port = port.value();
  client_config.client_name = "lqf-cli";
  Outcome<FabricClient> client = FabricClient::connect(client_config);
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Status status = client.value().handshake();
  if (!status.ok()) {
    return report_failure(status);
  }

  const std::shared_ptr<Clock> clock = make_system_clock();
  const ClockReading now = clock->now();
  CapabilityDeclaration declaration;
  declaration.source = source.value();
  declaration.link_scope = link.value();
  declaration.transport = TransportKind::Synthetic;
  declaration.evidence_class = EvidenceClass::Synthetic;
  declaration.declared_at = Timestamp{now.wall_nanos, false};
  MetricCapability capability;
  capability.metric = descriptor->id;
  capability.unit = descriptor->unit;
  capability.semantics = descriptor->semantics;
  capability.counter_width = descriptor->counter_width;
  capability.lanes_declared = false;
  capability.lane_count = 0;
  capability.validity_nanos = 0;
  declaration.metrics.push_back(capability);
  declaration.note = "generated by lqf emit; no hardware was measured";

  u64 declared_revision = 0;
  bool duplicate_declaration = false;
  Status declaration_status = Status::success();
  for (u64 attempt = 1; attempt <= kMaxRevisionAttempts; ++attempt) {
    declaration.revision = CapabilityRevision(attempt);
    Outcome<CapabilityAck> ack = client.value().declare_capability(declaration);
    if (ack.ok()) {
      declared_revision = attempt;
      duplicate_declaration = ack.value().duplicate != 0;
      declaration_status = Status::success();
      break;
    }
    declaration_status = ack.status();
    // A stored declaration with the same revision but different content, or a
    // newer stored revision, is resolved by declaring the next revision rather
    // than by pretending the stored one is ours.
    if (declaration_status.code() != StatusCode::IdMismatch &&
        declaration_status.code() != StatusCode::Fenced) {
      break;
    }
  }
  if (declared_revision == 0) {
    return report_failure(declaration_status);
  }

  std::cout << "emit link=" << render_link_identity(link.value())
            << " source=" << render_source_identity(source.value())
            << " metric=" << render_metric_id(descriptor->id) << "\n";
  std::cout << "  declared revision=" << text::format_u64(declared_revision)
            << " duplicate=" << (duplicate_declaration ? "true" : "false") << "\n";
  std::cout << "  evidence-class=synthetic transport=synthetic origin=lqf-cli/synthetic-generator"
               " producer=lqf-emit\n";

  // Sequence numbers are per stream and are never reused: the base comes from
  // this run's wall clock, so a second lqf emit against the same runtime
  // continues the generator stream instead of re-using an identity that was
  // already accepted with different content.
  const u64 sequence_base = static_cast<u64>(now.wall_nanos);
  u64 accepted = 0;
  u64 duplicates = 0;
  u64 rejected = 0;
  for (u64 index = 0; index < count; ++index) {
    Observation observation;
    observation.link = link.value();
    observation.source = source.value();
    observation.sequence = SequenceNumber(sequence_base + index);
    observation.metric = descriptor->id;
    observation.lane = LaneDimension::aggregate_dimension();
    observation.unit = descriptor->unit;
    GaugeReading gauge;
    gauge.value = synthetic_level(index);
    gauge.validity_nanos = 0;
    observation.reading = gauge;
    observation.observed_at = Timestamp{clock->now().wall_nanos, false};
    observation.authority = AuthorityRank(1);
    observation.provenance.transport = TransportKind::Synthetic;
    observation.provenance.evidence_class = EvidenceClass::Synthetic;
    observation.provenance.origin = "lqf-cli/synthetic-generator";
    observation.provenance.producer = "lqf-emit";
    observation.provenance.clock_synchronized = false;

    std::vector<Observation> batch;
    batch.push_back(std::move(observation));
    Outcome<BatchOutcome> outcome = client.value().ingest(batch);
    if (!outcome.ok()) {
      std::cout.flush();
      return report_failure(outcome.status());
    }
    accepted += outcome.value().accepted;
    duplicates += outcome.value().duplicates;
    rejected += outcome.value().rejected;
    if (outcome.value().rejected != 0) {
      std::cout.flush();
      return report_failure(outcome.value().first_code,
                            std::string("the runtime rejected a generated observation: ") +
                                to_string(outcome.value().first_code) + "/" +
                                to_string(outcome.value().first_reason));
    }
    if (interval_millis != 0 && index + 1U < count) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<i64>(interval_millis)));
    }
  }

  std::cout << "  sequences=" << text::format_u64(sequence_base) << ".."
            << text::format_u64(sequence_base + count - 1U) << "\n";
  std::cout << "  observations=" << text::format_u64(count)
            << " accepted=" << text::format_u64(accepted)
            << " duplicates=" << text::format_u64(duplicates)
            << " rejected=" << text::format_u64(rejected)
            << " synthetic-acknowledged="
            << (parsed.args.has("--synthetic") ? "true" : "false") << "\n";
  std::cout << "  every emitted reading is generated (synthetic); nothing here is a measurement "
               "of hardware\n";
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

}  // namespace lqf::cli
