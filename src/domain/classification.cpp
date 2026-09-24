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

#include "lqf/domain/classification.hpp"

#include <algorithm>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

void append_indent(std::string& out, std::size_t indent) {
  out.append(indent, ' ');
}

void append_pair(std::string& out, std::size_t indent, const char* key, const std::string& value) {
  append_indent(out, indent);
  out.append(key);
  out.append(": ");
  out.append(value);
  out.push_back('\n');
}

std::string render_lane(const LaneDimension& lane) {
  if (lane.is_aggregate()) {
    return "aggregate";
  }
  return "lane" + text::format_u64(lane.value().value());
}

std::string render_double_value(double value) { return text::format_double(value); }

}  // namespace

std::string render_quality_state(QualityState state) { return std::string(to_string(state)); }

std::string render_evidence_record(const EvidenceRecord& record) {
  const Observation& observation = record.observation;
  std::string out;
  out.append("evidence ");
  out.append(render_link_identity(observation.link));
  out.push_back(' ');
  out.append(render_source_identity(observation.source));
  out.append(" seq=");
  out.append(text::format_u64(observation.sequence.value()));
  out.push_back(' ');
  out.append(render_metric_id(observation.metric));
  out.append(" lane=");
  out.append(render_lane(observation.lane));
  out.append(" value=");
  out.append(render_reading(observation.reading, observation.unit));
  out.append(" observed=");
  out.append(text::format_i64(observation.observed_at.unix_nanos));
  out.append(observation.observed_at.synchronized ? "s" : "u");
  out.append(" received=");
  out.append(text::format_i64(record.received.wall_nanos));
  out.append(" origin=");
  out.append(to_string(record.origin));
  out.append(" class=");
  out.append(to_string(observation.provenance.evidence_class));
  out.append(" authority=");
  out.append(text::format_u64(observation.authority.value()));
  return out;
}

std::string render_assessment(const MetricAssessment& assessment, std::size_t indent) {
  std::string out;
  append_indent(out, indent);
  out.append("metric ");
  out.append(render_metric_id(assessment.metric));
  out.append(" lane=");
  out.append(render_lane(assessment.lane));
  out.push_back('\n');
  append_pair(out, indent + 2, "state", to_string(assessment.state));
  append_pair(out, indent + 2, "confidence", to_string(assessment.confidence));
  append_pair(out, indent + 2, "reasons", render_reasons(assessment.reasons));
  append_pair(out, indent + 2, "capability-declared",
              assessment.capability_declared ? "true" : "false");
  append_pair(out, indent + 2, "sources",
              text::format_u64(assessment.sources_declared) + " declared, " +
                  text::format_u64(assessment.sources_with_evidence) + " with evidence, " +
                  text::format_u64(assessment.sources_fresh) + " fresh");
  if (assessment.value.has_value()) {
    append_pair(out, indent + 2, "value", render_double_value(*assessment.value));
  }
  if (assessment.rate_per_second.has_value()) {
    append_pair(out, indent + 2, "rate-per-second", render_double_value(*assessment.rate_per_second));
  }
  if (assessment.interval_rate_per_second.has_value()) {
    append_pair(out, indent + 2, "interval-rate-per-second (not a current rate)",
                render_double_value(*assessment.interval_rate_per_second));
  }
  if (assessment.disagreement.has_value()) {
    append_pair(out, indent + 2, "disagreement", render_double_value(*assessment.disagreement));
  }
  if (!assessment.rules.empty()) {
    std::string rules;
    for (const RuleId& rule : assessment.rules) {
      if (!rules.empty()) {
        rules.push_back(',');
      }
      rules.append(rule.value());
    }
    append_pair(out, indent + 2, "rules", rules);
  }
  if (!assessment.missing_lanes.empty()) {
    std::string lanes;
    for (const LaneId& lane : assessment.missing_lanes) {
      if (!lanes.empty()) {
        lanes.push_back(',');
      }
      lanes.append(text::format_u64(lane.value()));
    }
    append_pair(out, indent + 2, "missing-lanes", lanes);
  }
  for (const EvidenceRef& reference : assessment.evidence) {
    append_indent(out, indent + 2);
    out.append("evidence ");
    out.append(render_source_identity(reference.source));
    out.append(" seq=");
    out.append(text::format_u64(reference.sequence.value()));
    out.push_back(' ');
    out.append(render_metric_id(reference.metric));
    out.append(" lane=");
    out.append(render_lane(reference.lane));
    out.append(" value=");
    out.append(reference.rendered);
    out.append(" origin=");
    out.append(to_string(reference.origin));
    out.append(" class=");
    out.append(to_string(reference.evidence_class));
    out.append(" authority=");
    out.append(text::format_u64(reference.authority.value()));
    if (!reference.source.id.is_empty()) {
      out.append(" provenance=");
      out.append(reference.provenance.empty() ? std::string("unknown") : reference.provenance);
    }
    out.append(" fresh=");
    out.append(reference.fresh ? "true" : "false");
    out.append(" used=");
    out.append(reference.used ? "true" : "false");
    out.append(" age-ns=");
    out.append(text::format_i64(reference.age_nanos));
    out.push_back('\n');
  }
  return out;
}

std::string render_report(const LinkQualityReport& report) {
  std::string out;
  out.append("link-quality ");
  out.append(render_link_identity(report.link));
  out.push_back('\n');
  append_pair(out, 2, "overall", to_string(report.overall));
  append_pair(out, 2, "confidence", to_string(report.confidence));
  append_pair(out, 2, "reasons", render_reasons(report.reasons));
  append_pair(out, 2, "registered", report.link_registered ? "true" : "false");
  append_pair(out, 2, "evidence-present", report.evidence_present ? "true" : "false");
  append_pair(out, 2, "policy", report.policy.id.value() + "/gen" +
                                       text::format_u64(report.policy.generation.value()) + " " +
                                       report.policy.content_hash);
  append_pair(out, 2, "epoch", text::format_u64(report.epoch.value()));
  append_pair(out, 2, "evidence-counts",
              "considered=" + text::format_u64(report.evidence_considered) + " fresh=" +
                  text::format_u64(report.evidence_fresh) + " recovered=" +
                  text::format_u64(report.evidence_recovered));
  append_pair(out, 2, "generated-wall-nanos", text::format_i64(report.generated_wall_nanos));
  if (report.truncated) {
    append_pair(out, 2, "truncated", "true");
  }
  for (const MetricAssessment& assessment : report.metrics) {
    out.append(render_assessment(assessment, 2));
  }
  return out;
}

std::string render_inspection(const InspectionReport& report) {
  std::string out;
  out.append("inspection epoch=");
  out.append(text::format_u64(report.epoch.value()));
  out.append(" policy=");
  out.append(report.policy.id.value());
  out.append("/gen");
  out.append(text::format_u64(report.policy.generation.value()));
  out.push_back('\n');
  append_pair(out, 2, "links-inspected", text::format_u64(report.links_inspected));
  append_pair(out, 2, "entries-dropped", text::format_u64(report.entries_dropped));
  append_pair(out, 2, "truncated", report.truncated ? "true" : "false");
  append_pair(out, 2, "conflicts", text::format_u64(report.conflicts.size()));
  for (const ConflictEntry& conflict : report.conflicts) {
    append_indent(out, 4);
    out.append(render_link_identity(conflict.link));
    out.push_back(' ');
    out.append(render_metric_id(conflict.metric));
    out.append(" lane=");
    out.append(render_lane(conflict.lane));
    out.append(" spread=");
    out.append(render_double_value(conflict.spread));
    out.append(" tolerance=");
    out.append(render_double_value(conflict.tolerance));
    out.push_back('\n');
    for (const EvidenceRef& claimant : conflict.claimants) {
      append_indent(out, 6);
      out.append(render_source_identity(claimant.source));
      out.append(" seq=");
      out.append(text::format_u64(claimant.sequence.value()));
      out.append(" authority=");
      out.append(text::format_u64(claimant.authority.value()));
      out.append(" value=");
      out.append(claimant.rendered);
      out.push_back('\n');
    }
  }
  append_pair(out, 2, "stale", text::format_u64(report.stale.size()));
  for (const StaleEntry& entry : report.stale) {
    append_indent(out, 4);
    out.append(render_link_identity(entry.link));
    out.push_back(' ');
    out.append(render_metric_id(entry.metric));
    out.append(" lane=");
    out.append(render_lane(entry.lane));
    out.append(" source=");
    out.append(render_source_identity(entry.source));
    out.append(" origin=");
    out.append(to_string(entry.origin));
    out.append(" age-ns=");
    out.append(text::format_i64(entry.age_nanos));
    out.append(" reason=");
    out.append(to_string(entry.reason));
    out.push_back('\n');
  }
  append_pair(out, 2, "freshness", text::format_u64(report.freshness.size()));
  for (const FreshnessEntry& entry : report.freshness) {
    append_indent(out, 4);
    out.append(render_link_identity(entry.link));
    out.push_back(' ');
    out.append(render_metric_id(entry.metric));
    out.append(" lane=");
    out.append(render_lane(entry.lane));
    out.append(" fresh=");
    out.append(text::format_u64(entry.fresh_sources));
    out.append(" stale=");
    out.append(text::format_u64(entry.stale_sources));
    out.append(" recovered=");
    out.append(text::format_u64(entry.recovered_sources));
    out.append(" declared=");
    out.append(text::format_u64(entry.declared_sources));
    out.append(" newest-age-ns=");
    out.append(text::format_i64(entry.newest_age_nanos));
    out.push_back('\n');
  }
  return out;
}

}  // namespace lqf
