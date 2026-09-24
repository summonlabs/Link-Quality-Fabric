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

#ifndef LQF_DOMAIN_COUNTER_HPP
#define LQF_DOMAIN_COUNTER_HPP

#include <optional>
#include <string>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/domain/identity.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/quality.hpp"
#include "lqf/export.hpp"

namespace lqf {

// What happened between the previous reading and this one. The vocabulary is
// deliberately narrow: every name is a claim this runtime can prove from a
// declared width and two readings, and nothing else is claimed.
enum class CounterEvent : u8 {
  Baseline = 0,           // first reading of an incarnation: no delta exists
  Advance,                // value >= previous: exact, non-negative delta
  Wrap,                   // decrease proven to be a modular wrap of a declared width
  Reset,                  // source declared a reset: re-baselined, no delta
  DecreaseUnproven,       // decrease that cannot be proven to be a wrap: no delta
  WidthChanged,           // declared width changed: continuity broken, no delta
  SourceReincarnated,     // different source incarnation: continuity broken
  LinkGenerationChanged,  // different link generation: continuity broken
  OutOfRange,             // value does not fit the declared width
  SequenceRegressed,      // reading is older than the accepted watermark
  Count,
};

LQF_API const char* to_string(CounterEvent event) noexcept;
LQF_API ReasonCode reason_for(CounterEvent event) noexcept;
LQF_API bool counter_event_preserves_continuity(CounterEvent event) noexcept;

// A proven difference between two readings.
struct CounterDelta {
  u64 delta{0};
  i64 elapsed_nanos{0};
  bool spans_gap{false};   // the interval exceeds the declared continuity gap
  bool from_wrap{false};   // the difference includes a modular wrap

  friend bool operator==(const CounterDelta&, const CounterDelta&) = default;

  // Average rate over the exact interval the delta covers. Absent when the
  // interval is too short to divide by, so no infinity is ever produced.
  [[nodiscard]] std::optional<double> rate_per_second(i64 min_interval_nanos) const;
};

struct CounterStep {
  CounterEvent event{CounterEvent::Baseline};
  ReasonCode reason{ReasonCode::None};
  std::optional<CounterDelta> delta{};
  bool continuity_intact{false};
};

struct CounterObservation {
  u64 value{0};
  CounterWidth width{CounterWidth::Unspecified};
  bool reset_declared{false};
  i64 observed_nanos{0};
  i64 received_steady_nanos{0};
  SequenceNumber sequence{};
  SourceIncarnation incarnation{};
  LinkGeneration link_generation{};
};

struct CounterContinuityConfig {
  bool wrap_inference{false};
  double wrap_ceiling_fraction{0.75};
  double wrap_floor_fraction{0.25};
  i64 max_gap_nanos{60'000'000'000LL};
};

// Continuity tracker for one counter stream: one link generation, one source
// incarnation, one metric, one lane. The tracker never invents a delta: when
// continuity is broken it reports which claim failed and returns no delta.
class LQF_API CounterContinuity {
 public:
  CounterContinuity() = default;

  void clear() noexcept;
  CounterStep observe(const CounterObservation& reading, const CounterContinuityConfig& config);

  [[nodiscard]] bool has_baseline() const noexcept { return has_baseline_; }
  [[nodiscard]] CounterEvent last_event() const noexcept { return last_event_; }
  [[nodiscard]] const std::optional<CounterDelta>& last_delta() const noexcept { return last_delta_; }
  [[nodiscard]] u64 last_value() const noexcept { return last_value_; }
  [[nodiscard]] CounterWidth last_width() const noexcept { return last_width_; }
  [[nodiscard]] i64 last_observed_nanos() const noexcept { return last_observed_nanos_; }
  [[nodiscard]] SourceIncarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] LinkGeneration link_generation() const noexcept { return link_generation_; }

  [[nodiscard]] u64 samples() const noexcept { return samples_; }
  [[nodiscard]] u64 advances() const noexcept { return advances_; }
  [[nodiscard]] u64 wraps() const noexcept { return wraps_; }
  [[nodiscard]] u64 resets() const noexcept { return resets_; }
  [[nodiscard]] u64 unproven_decreases() const noexcept { return unproven_decreases_; }
  [[nodiscard]] u64 gaps() const noexcept { return gaps_; }
  [[nodiscard]] u64 reincarnations() const noexcept { return reincarnations_; }
  [[nodiscard]] u64 width_changes() const noexcept { return width_changes_; }
  [[nodiscard]] u64 out_of_range() const noexcept { return out_of_range_; }
  [[nodiscard]] u64 sequence_regressions() const noexcept { return sequence_regressions_; }
  [[nodiscard]] u64 missing_samples() const noexcept { return missing_samples_; }

  // True when a rate may be derived from the most recent step and the reading
  // is still inside the declared continuity gap.
  [[nodiscard]] bool rate_admissible() const noexcept { return rate_admissible_; }

 private:
  bool has_baseline_{false};
  u64 last_value_{0};
  CounterWidth last_width_{CounterWidth::Unspecified};
  bool last_reset_declared_{false};
  i64 last_observed_nanos_{0};
  i64 last_received_steady_nanos_{0};
  SequenceNumber last_sequence_{};
  SourceIncarnation incarnation_{};
  LinkGeneration link_generation_{};
  bool identity_bound_{false};
  CounterEvent last_event_{CounterEvent::Baseline};
  std::optional<CounterDelta> last_delta_{};
  bool rate_admissible_{false};

  u64 samples_{0};
  u64 advances_{0};
  u64 wraps_{0};
  u64 resets_{0};
  u64 unproven_decreases_{0};
  u64 gaps_{0};
  u64 reincarnations_{0};
  u64 width_changes_{0};
  u64 out_of_range_{0};
  u64 sequence_regressions_{0};
  u64 missing_samples_{0};
};

}  // namespace lqf

#endif  // LQF_DOMAIN_COUNTER_HPP
