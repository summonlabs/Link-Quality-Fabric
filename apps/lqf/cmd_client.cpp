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

// Read-only client commands: query, explain, inspect, metrics and policy.
//
// Every one of them prints exactly what the runtime returned. Where the wire
// protocol cannot carry an answer -- it has no policy-document and no
// capability-declaration request -- the command says so instead of inventing a
// value, and offers the offline journal replay path that can answer it.

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cmd_common.hpp"
#include "lqf/classify/explain.hpp"
#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"
#include "lqf/domain/capability.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/transport/client.hpp"

namespace lqf::cli {
namespace {

Outcome<FabricClient> connect_client(u16 port) {
  ClientConfig config;
  config.port = port;
  config.client_name = "lqf-cli";
  // Must match the server bound: an explanation reply is a long text field.
  config.codec.max_string_bytes = 4096;
  Outcome<FabricClient> client = FabricClient::connect(config);
  if (!client.ok()) {
    return client.status();
  }
  const Status handshake = client.value().handshake();
  if (!handshake.ok()) {
    return handshake;
  }
  return std::move(client);
}

CommandSpec shutdown_spec() {
  CommandSpec spec;
  spec.name = "shutdown";
  spec.summary = "send the Shutdown protocol message to a running runtime";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve (required)"},
      {"--token", true, "<token>", "shutdown token the runtime was armed with (required)"},
  };
  spec.required = {"--port", "--token"};
  return spec;
}

CommandSpec query_spec() {
  CommandSpec spec;
  spec.name = "query";
  spec.summary = "print the quality report of one link";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve (required)"},
      {"--link", true, "<id>", "link identity, optionally <id>/gen<n> (required)"},
      {"--lane", true, "<n>", "restrict the report to one lane"},
      {"--json", false, "", "print the report as deterministic JSON instead of text"},
  };
  spec.required = {"--port", "--link"};
  return spec;
}

CommandSpec explain_spec() {
  CommandSpec spec;
  spec.name = "explain";
  spec.summary = "print the derivation of one link's quality report";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve (required)"},
      {"--link", true, "<id>", "link identity, optionally <id>/gen<n> (required)"},
      {"--lane", true, "<n>", "restrict the derivation to one lane"},
  };
  spec.required = {"--port", "--link"};
  return spec;
}

CommandSpec inspect_spec() {
  CommandSpec spec;
  spec.name = "inspect";
  spec.summary = "print the fabric inspection report";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve (required)"},
      {"--link", true, "<id>", "restrict the inspection to one link"},
  };
  spec.required = {"--port"};
  return spec;
}

CommandSpec metrics_spec() {
  CommandSpec spec;
  spec.name = "metrics";
  spec.summary = "print the declared capability view of one link";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve"},
      {"--journal", true, "<path>", "journal to replay offline for the complete view"},
      {"--link", true, "<id>", "link identity, optionally <id>/gen<n> (required)"},
  };
  spec.required = {"--link"};
  return spec;
}

CommandSpec policy_spec() {
  CommandSpec spec;
  spec.name = "policy";
  spec.summary = "print the current policy generation and its document";
  spec.options = {
      {"--port", true, "<n>", "loopback port of a running lqf serve"},
      {"--journal", true, "<path>", "journal to replay offline for the full document"},
  };
  return spec;
}

// Exactly one endpoint selector, and it must be a known one.
int select_endpoint(const CommandSpec& spec, const Args& args, bool& offline,
                    std::string& journal_path) {
  const bool has_port = args.find("--port") != nullptr;
  const bool has_journal = args.find("--journal") != nullptr;
  if (has_port == has_journal) {
    return usage_error(spec, "exactly one of --port or --journal must be given");
  }
  offline = has_journal;
  if (has_journal) {
    journal_path = *args.find("--journal");
  }
  return kExitOk;
}

Outcome<QualityQuery> make_quality_query(const Args& args) {
  Outcome<LinkIdentity> link = parse_link_value(*args.find("--link"));
  if (!link.ok()) {
    return link.status();
  }
  QualityQuery request;
  request.link = link.value();
  request.include_evidence = true;
  if (const std::string* lane = args.find("--lane")) {
    Outcome<LaneDimension> dimension = parse_lane_value(*lane);
    if (!dimension.ok()) {
      return dimension.status();
    }
    request.lane = dimension.value();
  }
  return request;
}

}  // namespace

int run_shutdown(const std::vector<std::string>& argv) {
  const CommandSpec spec = shutdown_spec();
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
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Outcome<ShutdownAck> ack = client.value().shutdown(*parsed.args.find("--token"));
  if (!ack.ok()) {
    return report_failure(ack.status());
  }
  std::cout << "shutdown accepted=" << (ack.value().accepted != 0 ? "true" : "false") << " "
            << ack.value().detail << "\n";
  std::cout.flush();
  client.value().close();
  if (ack.value().accepted == 0) {
    return report_failure(StatusCode::Refused,
                          ack.value().detail.empty() ? "the runtime refused the shutdown request"
                                                     : ack.value().detail);
  }
  return kExitOk;
}

int run_query(const std::vector<std::string>& argv) {
  const CommandSpec spec = query_spec();
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
  Outcome<QualityQuery> request = make_quality_query(parsed.args);
  if (!request.ok()) {
    return usage_error(spec, request.status().message());
  }
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Outcome<LinkQualityReport> report = client.value().query(request.value());
  if (!report.ok()) {
    return report_failure(report.status());
  }
  if (parsed.args.has("--json")) {
    std::cout << render_report_json(report.value());
  } else {
    std::cout << render_report(report.value());
  }
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

int run_explain(const std::vector<std::string>& argv) {
  const CommandSpec spec = explain_spec();
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
  Outcome<QualityQuery> request = make_quality_query(parsed.args);
  if (!request.ok()) {
    return usage_error(spec, request.status().message());
  }
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Outcome<Explanation> explanation = client.value().explain(request.value());
  if (!explanation.ok()) {
    return report_failure(explanation.status());
  }
  std::cout << explanation.value().text;
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

int run_inspect(const std::vector<std::string>& argv) {
  const CommandSpec spec = inspect_spec();
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
  InspectionFilter filter;
  if (const std::string* link = parsed.args.find("--link")) {
    Outcome<LinkIdentity> identity = parse_link_value(*link);
    if (!identity.ok()) {
      return usage_error(spec, identity.status().message());
    }
    filter.link = identity.value();
  }
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Outcome<InspectionReport> report = client.value().inspect(filter);
  if (!report.ok()) {
    return report_failure(report.status());
  }
  std::cout << render_inspection(report.value());
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

int run_metrics(const std::vector<std::string>& argv) {
  const CommandSpec spec = metrics_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }
  bool offline = false;
  std::string journal_path;
  const int selection = select_endpoint(spec, parsed.args, offline, journal_path);
  if (selection != kExitOk) {
    return selection;
  }
  Outcome<LinkIdentity> link = parse_link_value(*parsed.args.find("--link"));
  if (!link.ok()) {
    return usage_error(spec, link.status().message());
  }

  if (offline) {
    Outcome<std::unique_ptr<OfflineJournalFabric>> offline_fabric =
        open_offline_journal(journal_path);
    if (!offline_fabric.ok()) {
      return report_failure(offline_fabric.status());
    }
    Fabric& fabric = offline_fabric.value()->fabric();
    Outcome<std::vector<MetricCapabilityView>> views = fabric.capabilities(link.value());
    if (!views.ok()) {
      return report_failure(views.status());
    }
    std::cout << "capability-view " << render_link_identity(link.value()) << " source=journal "
              << journal_path << "\n";
    const RecoveryReport& recovery = fabric.recovery_report();
    std::cout << "  replayed records=" << text::format_u64(recovery.records_read)
              << " torn-tail=" << (recovery.torn_tail ? "true" : "false")
              << " degraded=" << (recovery.degraded ? "true" : "false") << "\n";
    if (views.value().empty()) {
      std::cout << "  no metric is declared for this link\n";
      std::cout.flush();
      return kExitOk;
    }
    for (const MetricCapabilityView& view : views.value()) {
      std::cout << render_capability_view(view);
    }
    std::cout.flush();
    return kExitOk;
  }

  Outcome<u16> port = parse_port_value(*parsed.args.find("--port"), false);
  if (!port.ok()) {
    return usage_error(spec, port.status().message());
  }
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  // The declared capability view crosses the wire directly, so the tool reports
  // exactly what each source declared rather than inferring it from evidence.
  Outcome<std::vector<MetricCapabilityView>> views = client.value().capabilities(link.value());
  if (!views.ok()) {
    return report_failure(views.status());
  }
  std::cout << "capability-view " << render_link_identity(link.value())
            << " source=declarations\n";
  if (views.value().empty()) {
    std::cout << "  no metric is declared for this link\n";
    std::cout.flush();
    client.value().close();
    return kExitOk;
  }
  for (const MetricCapabilityView& view : views.value()) {
    std::cout << render_capability_view(view);
  }
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

int run_policy(const std::vector<std::string>& argv) {
  const CommandSpec spec = policy_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }
  bool offline = false;
  std::string journal_path;
  const int selection = select_endpoint(spec, parsed.args, offline, journal_path);
  if (selection != kExitOk) {
    return selection;
  }

  if (offline) {
    Outcome<std::unique_ptr<OfflineJournalFabric>> offline_fabric =
        open_offline_journal(journal_path);
    if (!offline_fabric.ok()) {
      return report_failure(offline_fabric.status());
    }
    Fabric& fabric = offline_fabric.value()->fabric();
    const PolicyStamp stamp = fabric.current_policy();
    std::cout << "policy " << stamp.id.value()
              << " generation=" << text::format_u64(stamp.generation.value())
              << " content-hash=" << stamp.content_hash << " source=journal " << journal_path
              << "\n";
    Outcome<PolicyGenerationRecord> record = fabric.policy_document(stamp.generation);
    if (!record.ok()) {
      return report_failure(record.status());
    }
    const std::string recomputed =
        Sha256::hex(Sha256::digest(canonical_policy_text(record.value().document)));
    if (recomputed != stamp.content_hash) {
      return report_failure(StatusCode::Corrupt,
                            "the replayed policy document does not match the published content "
                            "hash");
    }
    std::cout << "document verified against the published content hash\n";
    std::cout << render_policy_document(record.value().document);
    std::cout.flush();
    return kExitOk;
  }

  Outcome<u16> port = parse_port_value(*parsed.args.find("--port"), false);
  if (!port.ok()) {
    return usage_error(spec, port.status().message());
  }
  Outcome<FabricClient> client = connect_client(port.value());
  if (!client.ok()) {
    return report_failure(client.status());
  }
  Outcome<FabricStats> stats = client.value().stats();
  if (!stats.ok()) {
    return report_failure(stats.status());
  }
  const PolicyStamp stamp = stats.value().policy;
  std::cout << "policy " << stamp.id.value()
            << " generation=" << text::format_u64(stamp.generation.value())
            << " content-hash=" << stamp.content_hash << " source=server\n";
  // The published document crosses the wire with its generation. The content
  // hash is recomputed here and compared with the published stamp, so a document
  // that does not hash to the stamp is reported as corruption rather than shown.
  Outcome<PolicyGenerationRecord> record = client.value().policy_document(stamp.generation);
  if (!record.ok()) {
    std::cout.flush();
    client.value().close();
    return report_failure(record.status());
  }
  const std::string recomputed =
      Sha256::hex(Sha256::digest(canonical_policy_text(record.value().document)));
  if (recomputed != stamp.content_hash) {
    std::cout.flush();
    client.value().close();
    return report_failure(StatusCode::Corrupt,
                          "the received policy document does not hash to the published "
                          "content hash");
  }
  std::cout << "content-hash recomputed from the received document\n";
  std::cout << render_policy_document(record.value().document);
  std::cout.flush();
  client.value().close();
  return kExitOk;
}

}  // namespace lqf::cli
