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

#include "lqf/runtime/config.hpp"

#include <algorithm>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

Status require_between(const char* name, std::size_t value, std::size_t low, std::size_t high) {
  if (value < low || value > high) {
    return Status::error(StatusCode::Invalid,
                         std::string(name) + " must be within [" + std::to_string(low) + ", " +
                             std::to_string(high) + "], received " + std::to_string(value));
  }
  return Status::success();
}

}  // namespace

Status FabricLimits::validate() const {
  Status status = require_between("max_links", max_links, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_generations_per_link", max_generations_per_link, 1, 64);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_sources_per_link", max_sources_per_link, 1, 4096);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_streams_per_link", max_streams_per_link, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_capability_declarations", max_capability_declarations, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_metrics_per_capability", max_metrics_per_capability, 1, 4096);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_policy_rules", max_policy_rules, 1, 4096);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_bands_per_rule", max_bands_per_rule, 2, 256);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_policy_generations", max_policy_generations, 1, 4096);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_stream_history", max_stream_history, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_global_history", max_global_history, 1, 1U << 24U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_dedup_window", max_dedup_window, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_evidence_batch", max_evidence_batch, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_evidence_per_metric", max_evidence_per_metric, 1, 1024);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_metrics_per_query", max_metrics_per_query, 1, 4096);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_window_records", max_window_records, 1, 1U << 24U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_inspection_entries", max_inspection_entries, 1, 1U << 20U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_string_bytes", max_string_bytes, 8, 4096);
  if (!status.ok()) {
    return status;
  }
  if (max_observation_age_nanos < 0) {
    return Status::error(StatusCode::Invalid, "max_observation_age_nanos must not be negative");
  }
  if (max_dedup_window < max_stream_history) {
    return Status::error(StatusCode::Invalid,
                         "max_dedup_window must be at least max_stream_history so that replay "
                         "fencing covers every retained record");
  }
  return Status::success();
}

Status JournalConfig::validate() const {
  Status status = require_between("max_record_bytes", max_record_bytes, 64, 1U << 26U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_queue_depth", max_queue_depth, 1, 1U << 22U);
  if (!status.ok()) {
    return status;
  }
  status = require_between("max_recovery_records", max_recovery_records, 1, 1U << 26U);
  if (!status.ok()) {
    return status;
  }
  if (enabled && path.empty()) {
    return Status::error(StatusCode::Invalid, "a journal path is required when the journal is on");
  }
  if (path.size() > 512) {
    return Status::error(StatusCode::Invalid, "journal path is too long");
  }
  if (compact_after_records == 0 && compact_after_bytes == 0) {
    return Status::error(StatusCode::Invalid,
                         "automatic compaction needs at least one trigger; both are zero");
  }
  return Status::success();
}

}  // namespace lqf
