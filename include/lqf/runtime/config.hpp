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

#ifndef LQF_RUNTIME_CONFIG_HPP
#define LQF_RUNTIME_CONFIG_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/clock.hpp"
#include "lqf/core/status.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Every bound the runtime enforces. Nothing grows without a declared limit, and
// every limit is validated at startup so a nonsensical configuration fails
// loudly instead of at the first oversized input.
struct FabricLimits {
  std::size_t max_links{4096};
  std::size_t max_generations_per_link{4};
  std::size_t max_sources_per_link{64};
  std::size_t max_streams_per_link{1024};
  std::size_t max_capability_declarations{1024};
  std::size_t max_metrics_per_capability{128};
  std::size_t max_policy_rules{128};
  std::size_t max_bands_per_rule{16};
  std::size_t max_policy_generations{8};
  std::size_t max_stream_history{64};
  std::size_t max_global_history{65536};
  std::size_t max_dedup_window{256};
  std::size_t max_evidence_batch{1024};
  std::size_t max_evidence_per_metric{8};
  std::size_t max_metrics_per_query{64};
  std::size_t max_window_records{4096};
  std::size_t max_inspection_entries{256};
  std::size_t max_string_bytes{128};
  // Evidence stamped by a clock-synchronized source that is older than this is
  // refused as stale instead of being admitted as late data. Zero disables the
  // check, which is the honest setting for sources with unsynchronized clocks.
  i64 max_observation_age_nanos{300'000'000'000LL};

  [[nodiscard]] Status validate() const;
};

enum class QueueOverflowPolicy : u8 {
  // Refuse the new record and report it. Durability is never silently traded
  // for availability.
  Reject = 0,
};

struct JournalConfig {
  bool enabled{false};
  std::string path{};
  std::size_t max_record_bytes{1U << 20U};
  std::size_t max_queue_depth{8192};
  QueueOverflowPolicy overflow{QueueOverflowPolicy::Reject};
  bool fsync_on_flush{true};
  u64 compact_after_bytes{256ULL << 20U};
  u64 compact_after_records{1'000'000ULL};
  bool compact_on_open{false};
  // Recovery stops after this many records and reports a truncated recovery
  // rather than reading an unbounded file.
  std::size_t max_recovery_records{4'000'000};
  std::size_t max_recovery_bytes{1ULL << 31U};

  [[nodiscard]] Status validate() const;
};

struct FabricConfig {
  FabricLimits limits{};
  JournalConfig journal{};
  std::shared_ptr<Clock> clock{};
  PolicyDocument initial_policy{};
  bool has_initial_policy{false};
  // Zero means "derive an incarnation from the clock and the process id".
  FabricEpoch forced_epoch{};
  std::size_t max_policy_generations{8};
};

}  // namespace lqf

#endif  // LQF_RUNTIME_CONFIG_HPP
