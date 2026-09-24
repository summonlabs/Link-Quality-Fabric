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

#include "lqf/domain/capability.hpp"

#include <algorithm>
#include <set>

#include "lqf/core/text.hpp"

namespace lqf {

Status validate_capability(const CapabilityDeclaration& declaration, const MetricCatalog& catalog,
                           const ObservationLimits& limits, std::size_t max_metrics) {
  Status status = validate_source_identity(declaration.source);
  if (!status.ok()) {
    return status;
  }
  if (declaration.link_scope.has_value()) {
    status = validate_link_identity(*declaration.link_scope);
    if (!status.ok()) {
      return status;
    }
  }
  if (declaration.revision.is_zero()) {
    return Status::error(StatusCode::Invalid, "capability revision must be at least 1");
  }
  if (declaration.metrics.size() > max_metrics) {
    return Status::error(StatusCode::LimitExceeded,
                         "capability declaration exceeds the metric limit of " +
                             std::to_string(max_metrics));
  }
  if (declaration.note.size() > limits.max_origin_bytes) {
    return Status::error(StatusCode::Invalid, "capability note is too long");
  }

  std::set<MetricId> seen;
  for (const MetricCapability& capability : declaration.metrics) {
    status = validate_metric_id(capability.metric);
    if (!status.ok()) {
      return status;
    }
    if (!seen.insert(capability.metric).second) {
      return Status::error(StatusCode::Invalid,
                           "duplicate metric in capability declaration: " +
                               render_metric_id(capability.metric));
    }
    if (!catalog.contains(capability.metric)) {
      return Status::error(StatusCode::Unsupported,
                           "metric is not registered in the catalog: " +
                               render_metric_id(capability.metric));
    }
    if (capability.unit == Unit::None) {
      return Status::error(StatusCode::Invalid,
                           "capability must declare units: " +
                               render_metric_id(capability.metric));
    }
    // The catalog is the authority on the unit of a metric identity: a source
    // cannot redefine what "rx.level" is measured in.
    const MetricDescriptor* descriptor = catalog.find(capability.metric);
    if (descriptor != nullptr && descriptor->unit != capability.unit) {
      return Status::error(StatusCode::Conflict,
                           "declared unit " + std::string(to_string(capability.unit)) +
                               " conflicts with the catalog unit " +
                               std::string(to_string(descriptor->unit)) + " for " +
                               render_metric_id(capability.metric));
    }
    if (capability.semantics == SampleSemantics::Counter) {
      if (capability.unit != Unit::Count && capability.unit != Unit::Bytes) {
        return Status::error(StatusCode::Invalid,
                             "counter capability must be declared in count or bytes: " +
                                 render_metric_id(capability.metric));
      }
    } else if (capability.unit == Unit::Count || capability.unit == Unit::Bytes) {
      return Status::error(StatusCode::Invalid,
                           "gauge capability must not be declared in count or bytes: " +
                               render_metric_id(capability.metric));
    }
    if (capability.lanes_declared) {
      if (capability.lane_count == 0 || capability.lane_count > limits.max_lanes) {
        return Status::error(StatusCode::Invalid,
                             "declared lane count is out of range: " +
                                 std::to_string(capability.lane_count));
      }
    } else if (capability.lane_count != 0) {
      return Status::error(StatusCode::Invalid,
                           "lane count declared without lane support: " +
                               render_metric_id(capability.metric));
    }
    if (capability.validity_nanos < 0) {
      return Status::error(StatusCode::Invalid, "declared validity window must not be negative");
    }
  }

  std::set<MetricId> unsupported;
  for (const MetricId& metric : declaration.unsupported_metrics) {
    status = validate_metric_id(metric);
    if (!status.ok()) {
      return status;
    }
    if (!unsupported.insert(metric).second) {
      return Status::error(StatusCode::Invalid,
                           "duplicate unsupported metric: " + render_metric_id(metric));
    }
    if (seen.find(metric) != seen.end()) {
      return Status::error(StatusCode::Conflict,
                           "metric is declared both measured and unsupported: " +
                               render_metric_id(metric));
    }
  }
  if (declaration.metrics.empty() && declaration.unsupported_metrics.empty()) {
    return Status::error(StatusCode::Invalid,
                         "capability declaration must declare or deny at least one metric");
  }
  return Status::success();
}

}  // namespace lqf
