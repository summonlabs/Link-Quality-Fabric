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

#include "harness.hpp"

#include <limits>

using namespace lqf;
using namespace lqf::test;

namespace {

CounterContinuityConfig wrap_config() {
  CounterContinuityConfig config;
  config.wrap_inference = true;
  config.wrap_ceiling_fraction = 0.75;
  config.wrap_floor_fraction = 0.25;
  config.max_gap_nanos = 1'000'000'000LL;
  return config;
}

CounterContinuityConfig plain_config() {
  CounterContinuityConfig config;
  config.wrap_inference = false;
  config.max_gap_nanos = 1'000'000'000LL;
  return config;
}

CounterObservation reading(u64 value, CounterWidth width, i64 observed, u64 sequence,
                           bool reset = false) {
  CounterObservation observation;
  observation.value = value;
  observation.width = width;
  observation.reset_declared = reset;
  observation.observed_nanos = observed;
  observation.received_steady_nanos = observed;
  observation.sequence = SequenceNumber(sequence);
  observation.incarnation = SourceIncarnation(1);
  observation.link_generation = LinkGeneration(1);
  return observation;
}

}  // namespace

LQF_TEST(counter_semantics, baseline_then_advance) {
  CounterContinuity continuity;
  const CounterStep first = continuity.observe(reading(100, CounterWidth::Bits64, 0, 1), plain_config());
  LQF_CHECK(first.event == CounterEvent::Baseline);
  LQF_CHECK(!first.delta.has_value());
  LQF_CHECK(first.reason == ReasonCode::CounterBaselineOnly);
  LQF_CHECK(!continuity.rate_admissible());

  const CounterStep second =
      continuity.observe(reading(175, CounterWidth::Bits64, 1'000'000'000LL, 2), plain_config());
  LQF_CHECK(second.event == CounterEvent::Advance);
  LQF_CHECK(second.delta.has_value());
  LQF_CHECK_EQ(second.delta->delta, u64{75});
  LQF_CHECK_EQ(second.delta->elapsed_nanos, 1'000'000'000LL);
  LQF_CHECK(!second.delta->spans_gap);
  LQF_CHECK(!second.delta->from_wrap);
  LQF_CHECK_EQ(second.delta->rate_per_second(0).value(), 75.0);
  LQF_CHECK(continuity.rate_admissible());
}

LQF_TEST(counter_semantics, monotonic_equal_reading_yields_zero_not_a_gap) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(500, CounterWidth::Bits64, 0, 1), plain_config());
  const CounterStep step =
      continuity.observe(reading(500, CounterWidth::Bits64, 1'000'000'000LL, 2), plain_config());
  LQF_CHECK(step.event == CounterEvent::Advance);
  LQF_CHECK_EQ(step.delta->delta, u64{0});
}

LQF_TEST(counter_semantics, decrease_without_wrap_proof_never_fabricates_a_delta) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(1'000'000, CounterWidth::Bits64, 0, 1), plain_config());
  const CounterStep step =
      continuity.observe(reading(5, CounterWidth::Bits64, 1'000'000'000LL, 2), plain_config());
  LQF_CHECK(step.event == CounterEvent::DecreaseUnproven);
  LQF_CHECK(!step.delta.has_value());
  LQF_CHECK(step.reason == ReasonCode::CounterDecreaseAmbiguous);
  LQF_CHECK(!step.continuity_intact);
  // The new reading is the baseline for the next step.
  const CounterStep next =
      continuity.observe(reading(9, CounterWidth::Bits64, 2'000'000'000LL, 3), plain_config());
  LQF_CHECK(next.event == CounterEvent::Advance);
  LQF_CHECK_EQ(next.delta->delta, u64{4});
}

LQF_TEST(counter_semantics, wrap_is_inferred_only_at_the_proven_boundary) {
  const u64 modulus = 1ULL << 32U;
  CounterContinuity continuity;
  (void)continuity.observe(reading(modulus - 10, CounterWidth::Bits32, 0, 1), wrap_config());
  const CounterStep wrapped =
      continuity.observe(reading(5, CounterWidth::Bits32, 1'000'000'000LL, 2), wrap_config());
  LQF_CHECK(wrapped.event == CounterEvent::Wrap);
  LQF_CHECK(wrapped.delta.has_value());
  LQF_CHECK_EQ(wrapped.delta->delta, u64{15});
  LQF_CHECK(wrapped.delta->from_wrap);
  LQF_CHECK(wrapped.reason == ReasonCode::CounterWrapInferred);
  LQF_CHECK_EQ(continuity.wraps(), u64{1});

  // A decrease far from the ceiling is not a wrap, even with inference enabled.
  CounterContinuity mid;
  (void)mid.observe(reading(10'000, CounterWidth::Bits32, 0, 1), wrap_config());
  const CounterStep ambiguous =
      mid.observe(reading(4, CounterWidth::Bits32, 1'000'000'000LL, 2), wrap_config());
  LQF_CHECK(ambiguous.event == CounterEvent::DecreaseUnproven);
  LQF_CHECK(!ambiguous.delta.has_value());

  // A wrap that lands above the floor is not proven either: the counter could
  // simply have been reset and advanced.
  CounterContinuity high_landing;
  (void)high_landing.observe(reading(modulus - 10, CounterWidth::Bits32, 0, 1), wrap_config());
  const CounterStep unproven =
      high_landing.observe(reading(modulus / 2, CounterWidth::Bits32, 1'000'000'000LL, 2),
                           wrap_config());
  LQF_CHECK(unproven.event == CounterEvent::DecreaseUnproven);
  LQF_CHECK(!unproven.delta.has_value());

  // Without a declared width no wrap can ever be proven.
  CounterContinuity undeclared;
  (void)undeclared.observe(reading(modulus - 10, CounterWidth::Unspecified, 0, 1), wrap_config());
  const CounterStep no_width =
      undeclared.observe(reading(5, CounterWidth::Unspecified, 1'000'000'000LL, 2), wrap_config());
  LQF_CHECK(no_width.event == CounterEvent::DecreaseUnproven);
  LQF_CHECK(!no_width.delta.has_value());
}

LQF_TEST(counter_semantics, declared_widths_wrap_exactly) {
  // A 64-bit counter that wraps at 2^64 produces the exact modular difference.
  CounterContinuity continuity;
  (void)continuity.observe(reading(std::numeric_limits<u64>::max() - 3, CounterWidth::Bits64, 0, 1),
                           wrap_config());
  const CounterStep step = continuity.observe(reading(5, CounterWidth::Bits64, 1'000'000'000LL, 2),
                                              wrap_config());
  LQF_CHECK(step.event == CounterEvent::Wrap);
  LQF_CHECK_EQ(step.delta->delta, u64{9});
}

LQF_TEST(counter_semantics, declared_reset_rebaselines_without_a_delta) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(900'000, CounterWidth::Bits64, 0, 1), wrap_config());
  const CounterStep reset =
      continuity.observe(reading(0, CounterWidth::Bits64, 1'000'000'000LL, 2, true), wrap_config());
  LQF_CHECK(reset.event == CounterEvent::Reset);
  LQF_CHECK(!reset.delta.has_value());
  LQF_CHECK(reset.reason == ReasonCode::CounterReset);
  LQF_CHECK_EQ(continuity.resets(), u64{1});
  const CounterStep next =
      continuity.observe(reading(12, CounterWidth::Bits64, 2'000'000'000LL, 3), wrap_config());
  LQF_CHECK(next.event == CounterEvent::Advance);
  LQF_CHECK_EQ(next.delta->delta, u64{12});
}

LQF_TEST(counter_semantics, width_change_breaks_continuity) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(100, CounterWidth::Bits32, 0, 1), wrap_config());
  const CounterStep changed =
      continuity.observe(reading(200, CounterWidth::Bits64, 1'000'000'000LL, 2), wrap_config());
  LQF_CHECK(changed.event == CounterEvent::WidthChanged);
  LQF_CHECK(!changed.delta.has_value());
  LQF_CHECK(changed.reason == ReasonCode::CounterWidthChanged);
  LQF_CHECK_EQ(continuity.width_changes(), u64{1});
}

LQF_TEST(counter_semantics, value_outside_the_declared_width_is_refused) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(10, CounterWidth::Bits32, 0, 1), wrap_config());
  CounterObservation invalid = reading(0x1FFFFFFFFULL, CounterWidth::Bits32, 1'000'000'000LL, 2);
  const CounterStep step = continuity.observe(invalid, wrap_config());
  LQF_CHECK(step.event == CounterEvent::OutOfRange);
  LQF_CHECK(!step.delta.has_value());
  LQF_CHECK(!continuity.has_baseline());
  LQF_CHECK_EQ(continuity.out_of_range(), u64{1});
}

LQF_TEST(counter_semantics, reincarnation_and_generation_changes_reset_continuity) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(100, CounterWidth::Bits64, 0, 1), plain_config());

  CounterObservation reincarnated = reading(7, CounterWidth::Bits64, 1'000'000'000LL, 1);
  reincarnated.incarnation = SourceIncarnation(2);
  const CounterStep step = continuity.observe(reincarnated, plain_config());
  LQF_CHECK(step.event == CounterEvent::SourceReincarnated);
  LQF_CHECK(!step.delta.has_value());
  LQF_CHECK(step.reason == ReasonCode::CounterSourceReincarnated);
  LQF_CHECK(!continuity.rate_admissible());

  CounterObservation next_generation = reading(9, CounterWidth::Bits64, 2'000'000'000LL, 2);
  next_generation.incarnation = SourceIncarnation(2);
  next_generation.link_generation = LinkGeneration(2);
  const CounterStep generation_step = continuity.observe(next_generation, plain_config());
  LQF_CHECK(generation_step.event == CounterEvent::LinkGenerationChanged);
  LQF_CHECK(!generation_step.delta.has_value());
  LQF_CHECK_EQ(continuity.reincarnations(), u64{2});
}

LQF_TEST(counter_semantics, gap_beyond_the_declared_window_is_reported_not_hidden) {
  CounterContinuityConfig config = plain_config();
  config.max_gap_nanos = 1'000'000'000LL;
  CounterContinuity continuity;
  (void)continuity.observe(reading(100, CounterWidth::Bits64, 0, 1), config);
  const CounterStep step =
      continuity.observe(reading(400, CounterWidth::Bits64, 60'000'000'000LL, 2), config);
  LQF_CHECK(step.event == CounterEvent::Advance);
  LQF_CHECK(step.delta.has_value());
  LQF_CHECK_EQ(step.delta->delta, u64{300});
  LQF_CHECK(step.delta->spans_gap);
  LQF_CHECK(!continuity.rate_admissible());
  LQF_CHECK_EQ(continuity.gaps(), u64{1});
  // The interval average is still available, explicitly labelled.
  LQF_CHECK_EQ(step.delta->rate_per_second(0).value(), 5.0);
}

LQF_TEST(counter_semantics, missing_samples_are_counted) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(100, CounterWidth::Bits64, 0, 10), plain_config());
  const CounterStep step =
      continuity.observe(reading(200, CounterWidth::Bits64, 1'000'000'000LL, 15), plain_config());
  LQF_CHECK(step.event == CounterEvent::Advance);
  LQF_CHECK_EQ(continuity.missing_samples(), u64{4});
}

LQF_TEST(counter_semantics, regressed_sequence_never_rewinds_continuity) {
  CounterContinuity continuity;
  (void)continuity.observe(reading(100, CounterWidth::Bits64, 0, 10), plain_config());
  const CounterStep step =
      continuity.observe(reading(50, CounterWidth::Bits64, 1'000'000'000LL, 9), plain_config());
  LQF_CHECK(step.event == CounterEvent::SequenceRegressed);
  LQF_CHECK(!step.delta.has_value());
  LQF_CHECK(step.continuity_intact);
  LQF_CHECK_EQ(continuity.last_value(), u64{100});
  LQF_CHECK_EQ(continuity.sequence_regressions(), u64{1});
  const CounterStep next =
      continuity.observe(reading(140, CounterWidth::Bits64, 2'000'000'000LL, 11), plain_config());
  LQF_CHECK(next.event == CounterEvent::Advance);
  LQF_CHECK_EQ(next.delta->delta, u64{40});
}

LQF_TEST(counter_semantics, rate_needs_a_usable_interval) {
  CounterDelta delta;
  delta.delta = 10;
  delta.elapsed_nanos = 0;
  LQF_CHECK(!delta.rate_per_second(0).has_value());
  delta.elapsed_nanos = 500;
  LQF_CHECK(!delta.rate_per_second(1'000'000).has_value());
  delta.elapsed_nanos = 2'000'000;
  LQF_CHECK_EQ(delta.rate_per_second(1'000'000).value(), 5000.0);
}

LQF_TEST(counter_semantics, extreme_values_do_not_overflow) {
  CounterContinuity continuity;
  const u64 maximum = std::numeric_limits<u64>::max();
  (void)continuity.observe(reading(0, CounterWidth::Bits64, 0, 1), wrap_config());
  const CounterStep step = continuity.observe(
      reading(maximum, CounterWidth::Bits64, 1'000'000'000LL, 2), wrap_config());
  LQF_CHECK(step.event == CounterEvent::Advance);
  LQF_CHECK_EQ(step.delta->delta, maximum);
  const double rate = step.delta->rate_per_second(0).value();
  LQF_CHECK(std::isfinite(rate));
  LQF_CHECK(rate > 1.8e19);

  // A backward clock step must not invent a positive interval.
  CounterContinuity backwards;
  (void)backwards.observe(reading(100, CounterWidth::Bits64, 10'000'000'000LL, 1), plain_config());
  const CounterStep step_back =
      backwards.observe(reading(200, CounterWidth::Bits64, 1'000'000'000LL, 2), plain_config());
  LQF_CHECK(step_back.delta.has_value());
  LQF_CHECK_EQ(step_back.delta->delta, u64{100});
  LQF_CHECK(step_back.delta->spans_gap);
  LQF_CHECK(!backwards.rate_admissible());
}
