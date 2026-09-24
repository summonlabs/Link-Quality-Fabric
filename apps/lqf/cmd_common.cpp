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

#include "cmd_common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <optional>
#include <system_error>
#include <utility>

#include "lqf/core/checked.hpp"
#include "lqf/core/text.hpp"

namespace lqf::cli {
namespace {

constexpr u64 kMaxPort = 65535;

void append_line(std::string& out, const char* key, const std::string& value) {
  out.append("  ");
  out.append(key);
  out.append(" ");
  out.append(value);
  out.push_back('\n');
}

std::string indent(std::size_t depth) { return std::string(depth * 2U, ' '); }

std::string json_string(std::string_view value) {
  std::string out;
  out.push_back('"');
  for (const char character : value) {
    const auto raw = static_cast<unsigned char>(character);
    switch (character) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (raw < 0x20U) {
          out.append("\\u00");
          const char digits[] = "0123456789abcdef";
          out.push_back(digits[(raw >> 4U) & 0x0FU]);
          out.push_back(digits[raw & 0x0FU]);
        } else {
          out.push_back(character);
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string json_reason_array(const ReasonList& reasons) {
  std::string out = "[";
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    if (index != 0U) {
      out.append(", ");
    }
    out.append(json_string(to_string(reasons[index])));
  }
  out.push_back(']');
  return out;
}

std::string json_rule_array(const std::vector<RuleId>& rules) {
  std::string out = "[";
  for (std::size_t index = 0; index < rules.size(); ++index) {
    if (index != 0U) {
      out.append(", ");
    }
    out.append(json_string(rules[index].value()));
  }
  out.push_back(']');
  return out;
}

std::string json_lane_array(const std::vector<LaneId>& lanes) {
  std::string out = "[";
  for (std::size_t index = 0; index < lanes.size(); ++index) {
    if (index != 0U) {
      out.append(", ");
    }
    out.append(text::format_u64(lanes[index].value()));
  }
  out.push_back(']');
  return out;
}

std::string json_double(std::optional<double> value) {
  if (!value.has_value() || !std::isfinite(*value)) {
    return "null";
  }
  return text::format_double(*value);
}

std::string lane_label(const LaneDimension& lane) {
  if (lane.is_aggregate()) {
    return "aggregate";
  }
  return "lane" + text::format_u64(lane.value().value());
}

std::string policy_stamp_text(const PolicyStamp& stamp) {
  return stamp.id.value() + "/gen" + text::format_u64(stamp.generation.value()) + " " +
         stamp.content_hash;
}

}  // namespace

bool Args::has(std::string_view name) const {
  const std::string key(name);
  return values.find(key) != values.end() || flags.find(key) != flags.end();
}

const std::string* Args::find(std::string_view name) const {
  const auto entry = values.find(std::string(name));
  return entry == values.end() ? nullptr : &entry->second;
}

std::string Args::value_or(std::string_view name, std::string fallback) const {
  const std::string* found = find(name);
  return found == nullptr ? std::move(fallback) : *found;
}

ParseResult parse_arguments(const CommandSpec& spec, const std::vector<std::string>& argv) {
  ParseResult result;
  for (std::size_t index = 0; index < argv.size(); ++index) {
    const std::string& token = argv[index];
    if (token.size() < 3U || token[0] != '-' || token[1] != '-') {
      result.error = "unexpected argument: " + token;
      return result;
    }
    std::string name = token;
    std::string inline_value;
    bool has_inline_value = false;
    const std::size_t equals = token.find('=');
    if (equals != std::string::npos) {
      name = token.substr(0, equals);
      inline_value = token.substr(equals + 1U);
      has_inline_value = true;
    }
    const OptionSpec* option = nullptr;
    for (const OptionSpec& candidate : spec.options) {
      if (candidate.name == name) {
        option = &candidate;
        break;
      }
    }
    if (option == nullptr) {
      result.error = "unknown option: " + name;
      return result;
    }
    if (!option->takes_value) {
      if (has_inline_value) {
        result.error = "option " + name + " does not take a value";
        return result;
      }
      result.args.flags.insert(name);
      continue;
    }
    std::string value;
    if (has_inline_value) {
      value = inline_value;
    } else if (index + 1U < argv.size()) {
      value = argv[++index];
    } else {
      result.error = "option " + name + " requires a value";
      return result;
    }
    result.args.values[name] = std::move(value);
  }
  for (const std::string& required : spec.required) {
    if (result.args.values.find(required) == result.args.values.end()) {
      result.error = "missing required option: " + required;
      return result;
    }
  }
  result.ok = true;
  return result;
}

std::string usage_line(const CommandSpec& spec) {
  std::string out = "usage: lqf ";
  out.append(spec.name);
  for (const OptionSpec& option : spec.options) {
    const bool required =
        std::find(spec.required.begin(), spec.required.end(), option.name) != spec.required.end();
    out.push_back(' ');
    if (!required) {
      out.push_back('[');
    }
    out.append(option.name);
    if (option.takes_value) {
      out.push_back(' ');
      out.append(option.value_name);
    }
    if (!required) {
      out.push_back(']');
    }
  }
  out.push_back('\n');
  return out;
}

void print_usage(std::ostream& out, const CommandSpec& spec) {
  out << usage_line(spec);
  out << "  " << spec.summary << "\n";
}

void print_help(std::ostream& out, const CommandSpec& spec) {
  print_usage(out, spec);
  out << "options:\n";
  for (const OptionSpec& option : spec.options) {
    out << "  " << option.name;
    if (option.takes_value) {
      out << " " << option.value_name;
    }
    out << "  " << option.help << "\n";
  }
}

void print_top_level_usage(std::ostream& out) {
  out << "usage: lqf <command> [options]\n";
  out << "commands:\n";
  out << "  serve           run a FabricServer on 127.0.0.1 until it is stopped\n";
  out << "  shutdown        send the Shutdown protocol message to a running runtime\n";
  out << "  journal-verify  open a journal read-only and print the recovery report\n";
  out << "  journal-dump    print one line per journal record\n";
  out << "  emit            connect and ingest generated gauge readings\n";
  out << "  query           print the quality report of one link\n";
  out << "  explain         print the derivation of one link's quality report\n";
  out << "  inspect         print the fabric inspection report\n";
  out << "  metrics         print the declared capability view of one link\n";
  out << "  policy          print the current policy generation and document\n";
}

int usage_error(const CommandSpec& spec, const std::string& message) {
  std::cerr << "lqf: " << message << "\n";
  std::cerr << usage_line(spec);
  return kExitUsage;
}

int report_failure(const Status& status) {
  std::cerr << "lqf: error: " << status.to_text() << "\n";
  return status.ok() ? kExitOk : kExitFailure;
}

int report_failure(StatusCode code, const std::string& message) {
  return report_failure(Status::error(code, message));
}

Outcome<u64> parse_u64_value(const std::string& text_value, const char* what) {
  u64 parsed = 0;
  const Status status = text::parse_u64(text_value, parsed);
  if (!status.ok()) {
    return Status::error(StatusCode::Invalid,
                         std::string(what) + " is not an unsigned integer: " + text_value);
  }
  return parsed;
}

Outcome<u16> parse_port_value(const std::string& text_value, bool allow_zero) {
  Outcome<u64> parsed = parse_u64_value(text_value, "port");
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (parsed.value() > kMaxPort || (parsed.value() == 0 && !allow_zero)) {
    return Status::error(StatusCode::Invalid,
                         allow_zero ? "port must be within [0, 65535]: " + text_value
                                    : "port must be within [1, 65535]: " + text_value);
  }
  return static_cast<u16>(parsed.value());
}

Outcome<LinkIdentity> parse_link_value(const std::string& text_value) {
  LinkIdentity identity;
  std::string id = text_value;
  const std::size_t slash = text_value.rfind('/');
  if (slash != std::string::npos && text_value.compare(slash, 4U, "/gen") == 0) {
    u64 generation = 0;
    const std::string suffix = text_value.substr(slash + 4U);
    if (!text::parse_u64(suffix, generation).ok() || generation == 0) {
      return Status::error(StatusCode::Invalid, "link generation is malformed: " + text_value);
    }
    identity.generation = LinkGeneration(generation);
    id = text_value.substr(0, slash);
  }
  identity.id = LinkId(std::move(id));
  const Status status = validate_link_identity(identity);
  if (!status.ok()) {
    return status;
  }
  return identity;
}

Outcome<SourceIdentity> parse_source_value(const std::string& text_value) {
  SourceIdentity identity;
  std::string id = text_value;
  const std::size_t at = text_value.rfind('@');
  if (at != std::string::npos) {
    u64 incarnation = 0;
    const std::string suffix = text_value.substr(at + 1U);
    if (!text::parse_u64(suffix, incarnation).ok() || incarnation == 0) {
      return Status::error(StatusCode::Invalid, "source incarnation is malformed: " + text_value);
    }
    identity.incarnation = SourceIncarnation(incarnation);
    id = text_value.substr(0, at);
  }
  identity.id = SourceId(std::move(id));
  const Status status = validate_source_identity(identity);
  if (!status.ok()) {
    return status;
  }
  return identity;
}

Outcome<MetricId> parse_metric_value(const std::string& text_value) {
  const std::size_t colon = text_value.find(':');
  if (colon == std::string::npos) {
    return Status::error(StatusCode::Invalid,
                         "metric must be written as <family>:<name>, for example "
                         "signal-power:rx.level, received: " +
                             text_value);
  }
  MetricFamily family = MetricFamily::Unspecified;
  const Status family_status = parse_metric_family(text_value.substr(0, colon), family);
  if (!family_status.ok()) {
    return family_status;
  }
  MetricId id(family, text_value.substr(colon + 1U));
  const Status status = validate_metric_id(id);
  if (!status.ok()) {
    return status;
  }
  return id;
}

Outcome<LaneDimension> parse_lane_value(const std::string& text_value) {
  Outcome<u64> parsed = parse_u64_value(text_value, "lane");
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (parsed.value() > 0xFFFFFFFFULL) {
    return Status::error(StatusCode::Invalid, "lane is out of range: " + text_value);
  }
  return LaneDimension(LaneId(static_cast<u32>(parsed.value())));
}

std::string render_recovery_report(const RecoveryReport& report) {
  std::string out;
  append_line(out, "opened", report.opened ? "true" : "false");
  append_line(out, "header-valid", report.header_valid ? "true" : "false");
  append_line(out, "records-read", text::format_u64(report.records_read));
  append_line(out, "bytes-read", text::format_u64(report.bytes_read));
  append_line(out, "bytes-discarded", text::format_u64(report.bytes_discarded));
  append_line(out, "torn-tail", report.torn_tail ? "true" : "false");
  append_line(out, "degraded", report.degraded ? "true" : "false");
  append_line(out, "snapshot-loaded", report.snapshot_loaded ? "true" : "false");
  append_line(out, "truncated-file", report.truncated_file ? "true" : "false");
  append_line(out, "temp-discarded", report.temp_discarded ? "true" : "false");
  append_line(out, "limit-reached", report.limit_reached ? "true" : "false");
  append_line(out, "first-bad-offset", text::format_u64(report.first_bad_offset));
  append_line(out, "status", report.status.to_text());
  if (!report.detail.empty()) {
    append_line(out, "detail", report.detail);
  }
  return out;
}

std::string render_epoch_and_policy(const FabricEpoch& epoch, const PolicyStamp& policy) {
  return "epoch " + text::format_u64(epoch.value()) + " policy " + policy_stamp_text(policy);
}

std::string render_report_json(const LinkQualityReport& report) {
  std::string out;
  out.append("{\n");
  out.append(indent(1) + "\"link\": " + json_string(render_link_identity(report.link)) + ",\n");
  out.append(indent(1) + "\"link_registered\": " +
             (report.link_registered ? "true" : "false") + ",\n");
  out.append(indent(1) + "\"evidence_present\": " +
             (report.evidence_present ? "true" : "false") + ",\n");
  out.append(indent(1) + "\"overall\": " + json_string(to_string(report.overall)) + ",\n");
  out.append(indent(1) + "\"confidence\": " + json_string(to_string(report.confidence)) + ",\n");
  out.append(indent(1) + "\"reasons\": " + json_reason_array(report.reasons) + ",\n");
  out.append(indent(1) + "\"policy\": {\"id\": " + json_string(report.policy.id.value()) +
             ", \"generation\": " + text::format_u64(report.policy.generation.value()) +
             ", \"content_hash\": " + json_string(report.policy.content_hash) + "},\n");
  out.append(indent(1) + "\"epoch\": " + text::format_u64(report.epoch.value()) + ",\n");
  out.append(indent(1) + "\"evidence\": {\"considered\": " +
             text::format_u64(report.evidence_considered) +
             ", \"fresh\": " + text::format_u64(report.evidence_fresh) +
             ", \"recovered\": " + text::format_u64(report.evidence_recovered) + "},\n");
  out.append(indent(1) + "\"generated_wall_nanos\": " +
             text::format_i64(report.generated_wall_nanos) + ",\n");
  out.append(indent(1) + "\"generated_steady_nanos\": " +
             text::format_i64(report.generated_steady_nanos) + ",\n");
  out.append(indent(1) + "\"truncated\": " + (report.truncated ? "true" : "false") + ",\n");
  out.append(indent(1) + "\"metrics\": [");
  if (report.metrics.empty()) {
    out.append("]\n");
  } else {
    out.push_back('\n');
    for (std::size_t index = 0; index < report.metrics.size(); ++index) {
      const MetricAssessment& assessment = report.metrics[index];
      out.append(indent(2) + "{\n");
      out.append(indent(3) + "\"metric\": " + json_string(render_metric_id(assessment.metric)) +
                 ",\n");
      out.append(indent(3) + "\"lane\": " + json_string(lane_label(assessment.lane)) + ",\n");
      out.append(indent(3) + "\"state\": " + json_string(to_string(assessment.state)) + ",\n");
      out.append(indent(3) + "\"confidence\": " + json_string(to_string(assessment.confidence)) +
                 ",\n");
      out.append(indent(3) + "\"capability_declared\": " +
                 (assessment.capability_declared ? "true" : "false") + ",\n");
      out.append(indent(3) + "\"sources_declared\": " +
                 text::format_u64(assessment.sources_declared) + ",\n");
      out.append(indent(3) + "\"sources_with_evidence\": " +
                 text::format_u64(assessment.sources_with_evidence) + ",\n");
      out.append(indent(3) + "\"sources_fresh\": " +
                 text::format_u64(assessment.sources_fresh) + ",\n");
      out.append(indent(3) + "\"sources_conflicting\": " +
                 text::format_u64(assessment.sources_conflicting) + ",\n");
      out.append(indent(3) + "\"value\": " + json_double(assessment.value) + ",\n");
      out.append(indent(3) + "\"rate_per_second\": " + json_double(assessment.rate_per_second) +
                 ",\n");
      out.append(indent(3) + "\"interval_rate_per_second\": " +
                 json_double(assessment.interval_rate_per_second) + ",\n");
      out.append(indent(3) + "\"disagreement\": " + json_double(assessment.disagreement) + ",\n");
      out.append(indent(3) + "\"rules\": " + json_rule_array(assessment.rules) + ",\n");
      out.append(indent(3) + "\"reasons\": " + json_reason_array(assessment.reasons) + ",\n");
      out.append(indent(3) + "\"missing_lanes\": " + json_lane_array(assessment.missing_lanes) +
                 ",\n");
      out.append(indent(3) + "\"evidence\": [");
      if (assessment.evidence.empty()) {
        out.append("]\n");
      } else {
        out.push_back('\n');
        for (std::size_t evidence_index = 0; evidence_index < assessment.evidence.size();
             ++evidence_index) {
          const EvidenceRef& reference = assessment.evidence[evidence_index];
          out.append(indent(4) + "{");
          out.append("\"source\": " + json_string(render_source_identity(reference.source)));
          out.append(", \"sequence\": " + text::format_u64(reference.sequence.value()));
          out.append(", \"lane\": " + json_string(lane_label(reference.lane)));
          out.append(", \"unit\": " + json_string(to_string(reference.unit)));
          out.append(", \"origin\": " + json_string(to_string(reference.origin)));
          out.append(", \"evidence_class\": " +
                     json_string(to_string(reference.evidence_class)));
          out.append(", \"authority\": " + text::format_u64(reference.authority.value()));
          out.append(", \"observed_at_unix_nanos\": " +
                     text::format_i64(reference.observed_at.unix_nanos));
          out.append(", \"fresh\": " + std::string(reference.fresh ? "true" : "false"));
          out.append(", \"age_nanos\": " + text::format_i64(reference.age_nanos));
          out.append(", \"rendered\": " + json_string(reference.rendered));
          out.push_back('}');
          if (evidence_index + 1U != assessment.evidence.size()) {
            out.push_back(',');
          }
          out.push_back('\n');
        }
        out.append(indent(3) + "]\n");
      }
      out.append(indent(2) + "}");
      if (index + 1U != report.metrics.size()) {
        out.push_back(',');
      }
      out.push_back('\n');
    }
    out.append(indent(1) + "]\n");
  }
  out.append("}\n");
  return out;
}

std::string render_seconds(double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    return "0.000000";
  }
  const double micros = seconds * 1'000'000.0;
  const u64 rounded = static_cast<u64>(micros + 0.5);
  std::string out = text::format_u64(rounded / 1'000'000U);
  out.push_back('.');
  const u64 fraction = rounded % 1'000'000U;
  const std::string digits = text::format_u64(fraction);
  out.append(6U - digits.size(), '0');
  out.append(digits);
  return out;
}

OfflineJournalFabric::OfflineJournalFabric(std::shared_ptr<Fabric> fabric, std::filesystem::path copy)
    : fabric_(std::move(fabric)), copy_(std::move(copy)) {}

OfflineJournalFabric::~OfflineJournalFabric() {
  if (fabric_ != nullptr) {
    const Status status = fabric_->close();
    (void)status;
    fabric_.reset();
  }
  if (!copy_.empty()) {
    std::error_code error;
    std::filesystem::remove(copy_, error);
    (void)error;
  }
}

Outcome<std::unique_ptr<OfflineJournalFabric>> open_offline_journal(
    const std::string& journal_path) {
  if (journal_path.empty()) {
    return Status::error(StatusCode::Invalid, "a journal path is required");
  }
  if (!file_exists(journal_path)) {
    return Status::error(StatusCode::NotFound, "journal file does not exist: " + journal_path);
  }
  std::error_code error;
  const std::filesystem::path source(journal_path);
  const i64 stamp = static_cast<i64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path copy = std::filesystem::temp_directory_path(error);
  if (error) {
    return Status::error(StatusCode::Io,
                         "a temporary directory is not available: " + error.message());
  }
  copy /= "lqf-cli-" + text::format_i64(stamp) + ".journal";
  std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) {
    return Status::error(StatusCode::Io,
                         "the journal could not be copied for offline replay: " + error.message());
  }

  FabricConfig config;
  config.journal.enabled = true;
  config.journal.path = copy.string();
  config.clock = make_system_clock();
  Outcome<std::shared_ptr<Fabric>> opened = Fabric::open(std::move(config));
  if (!opened.ok()) {
    std::error_code removed;
    std::filesystem::remove(copy, removed);
    return opened.status();
  }
  return std::make_unique<OfflineJournalFabric>(std::move(opened.value()), copy);
}

}  // namespace lqf::cli
