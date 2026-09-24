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

#include "lqf/domain/observation.hpp"

#include "lqf/core/text.hpp"

namespace lqf {

Status validate_observation(const Observation& observation, const MetricDescriptor* descriptor,
                            const ObservationLimits& limits) {
  Status status = validate_link_identity(observation.link);
  if (!status.ok()) {
    return status;
  }
  status = validate_source_identity(observation.source);
  if (!status.ok()) {
    return status;
  }
  status = validate_metric_id(observation.metric);
  if (!status.ok()) {
    return status;
  }
  status = validate_provenance(observation.provenance, limits.max_origin_bytes);
  if (!status.ok()) {
    return status;
  }
  if (observation.unit == Unit::None) {
    return Status::error(StatusCode::Invalid,
                         "observation must declare explicit units: " +
                             render_metric_id(observation.metric));
  }
  if (!observation.lane.is_aggregate()) {
    status = validate_lane_id(observation.lane.value().value(), limits.max_lanes);
    if (!status.ok()) {
      return status;
    }
  }
  if (observation.interval_nanos.has_value() && *observation.interval_nanos <= 0) {
    return Status::error(StatusCode::Invalid, "declared interval must be positive");
  }

  if (const auto* gauge = as_gauge(observation.reading)) {
    if (!is_finite(gauge->value)) {
      return Status::error(StatusCode::Invalid,
                           "gauge reading is not finite: " +
                               render_metric_id(observation.metric));
    }
    if (gauge->validity_nanos < 0) {
      return Status::error(StatusCode::Invalid, "gauge validity window must not be negative");
    }
    if (observation.unit == Unit::Count || observation.unit == Unit::Bytes) {
      return Status::error(StatusCode::Invalid,
                           "gauge reading must not be declared in count or bytes: " +
                               render_metric_id(observation.metric));
    }
  } else if (const auto* counter = as_counter(observation.reading)) {
    if (observation.unit != Unit::Count && observation.unit != Unit::Bytes) {
      return Status::error(StatusCode::Invalid,
                           "counter reading must be declared in count or bytes: " +
                               render_metric_id(observation.metric));
    }
    if (!counter_value_in_range(counter->value, counter->width)) {
      return Status::error(StatusCode::Invalid,
                           "counter reading does not fit the declared width " +
                               std::string(to_string(counter->width)));
    }
  } else {
    return Status::error(StatusCode::Invalid, "observation carries no reading");
  }

  if (descriptor != nullptr) {
    if (descriptor->semantics != observation.semantics()) {
      return Status::error(StatusCode::Invalid,
                           "sample semantics do not match the declared metric: " +
                               render_metric_id(observation.metric));
    }
    if (descriptor->unit != observation.unit) {
      return Status::error(StatusCode::Invalid,
                           "units do not match the declared metric: expected " +
                               std::string(to_string(descriptor->unit)) + ", received " +
                               std::string(to_string(observation.unit)));
    }
    if (!descriptor->supports_lanes && !observation.lane.is_aggregate()) {
      return Status::error(StatusCode::Invalid,
                           "metric is not declared per lane: " +
                               render_metric_id(observation.metric));
    }
  }
  return Status::success();
}

std::string render_reading(const Reading& reading, Unit unit) {
  if (const auto* gauge = as_gauge(reading)) {
    std::string out = text::format_double(gauge->value);
    out.push_back(' ');
    out.append(to_string(unit));
    return out;
  }
  if (const auto* counter = as_counter(reading)) {
    std::string out = text::format_u64(counter->value);
    out.push_back(' ');
    out.append(to_string(unit));
    out.append(" (");
    out.append(to_string(counter->width));
    out.push_back(')');
    if (counter->reset_declared) {
      out.append(" reset-declared");
    }
    return out;
  }
  return "no-reading";
}

}  // namespace lqf
