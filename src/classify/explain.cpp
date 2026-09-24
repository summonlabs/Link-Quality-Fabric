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

#include "lqf/classify/explain.hpp"

#include <algorithm>

#include "lqf/core/text.hpp"

namespace lqf {

Explanation explain_link(const ClassifierContext& context, const QualityQuery& query) {
  Explanation explanation;
  explanation.report = classify_link(context, query);
  explanation.text = render_report(explanation.report);
  return explanation;
}

std::string render_policy_document(const PolicyDocument& document) {
  std::string out;
  out.append("policy ");
  out.append(document.id.value());
  out.append(" require-capability=");
  out.append(document.require_capability ? "true" : "false");
  out.push_back('\n');
  out.append("  freshness default-validity-ns=");
  out.append(text::format_i64(document.freshness.default_validity_nanos));
  out.append(" continuity-gap-ns=");
  out.append(text::format_i64(document.freshness.default_continuity_gap_nanos));
  out.append(" min-rate-interval-ns=");
  out.append(text::format_i64(document.freshness.min_rate_interval_nanos));
  out.push_back('\n');
  for (std::size_t index = 0; index < kQualityStateCount; ++index) {
    out.append("  severity ");
    out.append(to_string(static_cast<QualityState>(index)));
    out.push_back('=');
    out.append(text::format_u64(document.aggregation.severity.rank[index]));
    out.push_back('\n');
  }
  std::vector<MetricRule> rules = document.rules;
  std::sort(rules.begin(), rules.end(),
            [](const MetricRule& lhs, const MetricRule& rhs) { return lhs.metric < rhs.metric; });
  for (const MetricRule& rule : rules) {
    out.append("  rule ");
    out.append(render_metric_id(rule.metric));
    out.append(" basis=");
    out.append(to_string(rule.basis));
    out.append(" tolerance=");
    out.append(text::format_double(rule.conflict_tolerance));
    out.append(" validity-ns=");
    out.append(text::format_i64(rule.validity_nanos));
    out.append(" gap-ns=");
    out.append(text::format_i64(rule.max_continuity_gap_nanos));
    out.append(" wrap-inference=");
    out.append(rule.wrap_inference ? "true" : "false");
    out.push_back('\n');
    for (const ThresholdBand& band : rule.bands) {
      out.append("    band ");
      out.append(band.id.value());
      out.append(" [");
      out.append(text::format_double(band.lower));
      out.append(",");
      out.append(band.upper.has_value() ? text::format_double(*band.upper) : std::string("inf"));
      out.append(") -> ");
      out.append(to_string(band.state));
      out.push_back('\n');
    }
  }
  for (const MetricId& metric : document.unsupported_metrics) {
    out.append("  unsupported ");
    out.append(render_metric_id(metric));
    out.push_back('\n');
  }
  return out;
}

std::string render_capability_view(const MetricCapabilityView& view) {
  std::string out;
  out.append(render_metric_id(view.metric));
  out.append(" declared-by=");
  out.append(text::format_u64(view.declared_by.size()));
  out.append(" unsupported-by=");
  out.append(text::format_u64(view.declared_unsupported_by.size()));
  out.push_back('\n');
  for (const DeclaredSource& source : view.declared_by) {
    out.append("  ");
    out.append(render_source_identity(source.source));
    out.append(" rev=");
    out.append(text::format_u64(source.revision.value()));
    out.append(" unit=");
    out.append(to_string(source.unit));
    out.append(" semantics=");
    out.append(to_string(source.semantics));
    out.append(" width=");
    out.append(to_string(source.counter_width));
    out.append(" lanes=");
    out.append(source.lanes_declared ? text::format_u64(source.lane_count) : std::string("none"));
    out.append(" class=");
    out.append(to_string(source.evidence_class));
    out.append(" transport=");
    out.append(to_string(source.transport));
    out.push_back('\n');
  }
  for (const DeclaredSource& source : view.declared_unsupported_by) {
    out.append("  unsupported-by ");
    out.append(render_source_identity(source.source));
    out.push_back('\n');
  }
  return out;
}

}  // namespace lqf
