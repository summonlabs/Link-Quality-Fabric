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

#include "lqf/domain/counter.hpp"

#include <cmath>
#include <limits>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

// Threshold used by wrap inference, expressed as an absolute reading boundary.
// Computed in long double so a fraction of the full 64-bit modulus cannot
// overflow or underflow before it is clamped back into the reading domain.
u64 boundary(u64 modulus, double fraction, bool ceiling) {
  const long double scaled = static_cast<long double>(modulus) * static_cast<long double>(fraction);
  if (scaled <= 0.0L) {
    return 0;
  }
  const long double maximum = static_cast<long double>(std::numeric_limits<u64>::max());
  if (ceiling) {
    if (scaled >= maximum) {
      return std::numeric_limits<u64>::max();
    }
    return static_cast<u64>(scaled);
  }
  if (scaled >= maximum) {
    return std::numeric_limits<u64>::max();
  }
  return static_cast<u64>(scaled);
}

// The modulus as an unsigned value that wraps correctly in 64-bit arithmetic:
// a 64-bit counter wraps at 2^64, which is exactly what unsigned overflow does.
u64 modulus_value(CounterWidth width) {
  switch (width) {
    case CounterWidth::Bits32: return 1ULL << 32U;
    case CounterWidth::Bits64: return 0ULL;  // represents 2^64 through wrapping arithmetic
    case CounterWidth::Unspecified: return 0ULL;
  }
  return 0ULL;
}

struct DecreaseDecision {
  bool wrap_proven{false};
  u64 delta{0};
};

DecreaseDecision classify_decrease(u64 previous, u64 current, CounterWidth width,
                                  const CounterContinuityConfig& config) {
  DecreaseDecision decision;
  if (!config.wrap_inference || width == CounterWidth::Unspecified) {
    return decision;
  }
  const u64 modulus = modulus_value(width);
  const u64 effective_modulus = (width == CounterWidth::Bits64)
                                    ? std::numeric_limits<u64>::max()
                                    : modulus;
  const u64 ceiling = boundary(effective_modulus, config.wrap_ceiling_fraction, true);
  const u64 floor_bound = boundary(effective_modulus, config.wrap_floor_fraction, false);
  if (previous < ceiling || current > floor_bound) {
    return decision;
  }
  // Modular difference. A 32-bit counter wraps at 2^32, so the span is
  // (2^32 - previous) + current; a 64-bit counter wraps at 2^64, which is
  // exactly what unsigned subtraction already does.
  decision.wrap_proven = true;
  decision.delta = width == CounterWidth::Bits32 ? ((1ULL << 32U) - previous) + current
                                                 : current - previous;
  return decision;
}

}  // namespace

const char* to_string(CounterEvent event) noexcept {
  switch (event) {
    case CounterEvent::Baseline: return "baseline";
    case CounterEvent::Advance: return "advance";
    case CounterEvent::Wrap: return "wrap";
    case CounterEvent::Reset: return "reset";
    case CounterEvent::DecreaseUnproven: return "decrease-unproven";
    case CounterEvent::WidthChanged: return "width-changed";
    case CounterEvent::SourceReincarnated: return "source-reincarnated";
    case CounterEvent::LinkGenerationChanged: return "link-generation-changed";
    case CounterEvent::OutOfRange: return "out-of-range";
    case CounterEvent::SequenceRegressed: return "sequence-regressed";
    case CounterEvent::Count: break;
  }
  return "invalid";
}

ReasonCode reason_for(CounterEvent event) noexcept {
  switch (event) {
    case CounterEvent::Baseline: return ReasonCode::CounterBaselineOnly;
    case CounterEvent::Advance: return ReasonCode::None;
    case CounterEvent::Wrap: return ReasonCode::CounterWrapInferred;
    case CounterEvent::Reset: return ReasonCode::CounterReset;
    case CounterEvent::DecreaseUnproven: return ReasonCode::CounterDecreaseAmbiguous;
    case CounterEvent::WidthChanged: return ReasonCode::CounterWidthChanged;
    case CounterEvent::SourceReincarnated: return ReasonCode::CounterSourceReincarnated;
    case CounterEvent::LinkGenerationChanged: return ReasonCode::CounterLinkGenerationChanged;
    case CounterEvent::OutOfRange: return ReasonCode::CounterOutOfRange;
    case CounterEvent::SequenceRegressed: return ReasonCode::CounterSequenceRegressed;
    case CounterEvent::Count: break;
  }
  return ReasonCode::None;
}

bool counter_event_preserves_continuity(CounterEvent event) noexcept {
  return event == CounterEvent::Advance || event == CounterEvent::Wrap;
}

std::optional<double> CounterDelta::rate_per_second(i64 min_interval_nanos) const {
  if (elapsed_nanos <= 0 || elapsed_nanos < min_interval_nanos) {
    return std::nullopt;
  }
  const double seconds = static_cast<double>(elapsed_nanos) / 1'000'000'000.0;
  if (!(seconds > 0.0) || !std::isfinite(seconds)) {
    return std::nullopt;
  }
  const double rate = static_cast<double>(delta) / seconds;
  if (!std::isfinite(rate)) {
    return std::nullopt;
  }
  return rate;
}

void CounterContinuity::clear() noexcept { *this = CounterContinuity{}; }

CounterStep CounterContinuity::observe(const CounterObservation& reading,
                                       const CounterContinuityConfig& config) {
  CounterStep step;
  samples_ += 1;

  const auto rebaseline = [this, &reading]() {
    has_baseline_ = true;
    last_value_ = reading.value;
    last_width_ = reading.width;
    last_reset_declared_ = reading.reset_declared;
    last_observed_nanos_ = reading.observed_nanos;
    last_received_steady_nanos_ = reading.received_steady_nanos;
    last_sequence_ = reading.sequence;
    incarnation_ = reading.incarnation;
    link_generation_ = reading.link_generation;
    identity_bound_ = true;
  };

  if (identity_bound_) {
    if (reading.incarnation != incarnation_) {
      reincarnations_ += 1;
      rebaseline();
      last_event_ = CounterEvent::SourceReincarnated;
      last_delta_.reset();
      rate_admissible_ = false;
      step.event = last_event_;
      step.reason = reason_for(last_event_);
      step.continuity_intact = true;  // the new incarnation has a clean baseline
      return step;
    }
    if (reading.link_generation != link_generation_) {
      reincarnations_ += 1;
      rebaseline();
      last_event_ = CounterEvent::LinkGenerationChanged;
      last_delta_.reset();
      rate_admissible_ = false;
      step.event = last_event_;
      step.reason = reason_for(last_event_);
      step.continuity_intact = true;
      return step;
    }
  }

  if (!counter_value_in_range(reading.value, reading.width)) {
    out_of_range_ += 1;
    has_baseline_ = false;
    last_delta_.reset();
    rate_admissible_ = false;
    last_event_ = CounterEvent::OutOfRange;
    step.event = last_event_;
    step.reason = reason_for(last_event_);
    step.continuity_intact = false;
    return step;
  }

  if (has_baseline_ && reading.width != last_width_) {
    width_changes_ += 1;
    rebaseline();
    last_event_ = CounterEvent::WidthChanged;
    last_delta_.reset();
    rate_admissible_ = false;
    step.event = last_event_;
    step.reason = reason_for(last_event_);
    step.continuity_intact = true;
    return step;
  }

  if (has_baseline_ && reading.sequence.value() < last_sequence_.value()) {
    sequence_regressions_ += 1;
    // The stored baseline is newer than this reading: refuse the step without
    // disturbing the baseline, so a late sample can never rewind continuity.
    last_event_ = CounterEvent::SequenceRegressed;
    last_delta_.reset();
    rate_admissible_ = false;
    step.event = last_event_;
    step.reason = reason_for(last_event_);
    step.continuity_intact = true;
    return step;
  }

  if (reading.reset_declared) {
    resets_ += 1;
    rebaseline();
    last_event_ = CounterEvent::Reset;
    last_delta_.reset();
    rate_admissible_ = false;
    step.event = last_event_;
    step.reason = reason_for(last_event_);
    step.continuity_intact = true;
    return step;
  }

  if (!has_baseline_) {
    rebaseline();
    last_event_ = CounterEvent::Baseline;
    last_delta_.reset();
    rate_admissible_ = false;
    step.event = last_event_;
    step.reason = reason_for(last_event_);
    step.continuity_intact = true;
    return step;
  }

  if (has_baseline_ && reading.sequence.value() > last_sequence_.value() + 1U) {
    missing_samples_ += (reading.sequence.value() - last_sequence_.value() - 1U);
  }

  i64 elapsed = 0;
  if (!checked_sub_i64(reading.observed_nanos, last_observed_nanos_, elapsed)) {
    elapsed = std::numeric_limits<i64>::max();
  }
  if (elapsed < 0) {
    // The source clock moved backwards. The delta itself is still exactly two
    // readings apart in value, but the interval is not usable: report it as a
    // gap rather than inventing a duration.
    elapsed = 0;
  }
  const bool spans_gap = elapsed > config.max_gap_nanos || elapsed <= 0;

  CounterDelta delta;
  delta.elapsed_nanos = elapsed;
  delta.spans_gap = spans_gap;

  if (reading.value >= last_value_) {
    delta.delta = reading.value - last_value_;
    delta.from_wrap = false;
    advances_ += 1;
    last_event_ = CounterEvent::Advance;
  } else {
    const DecreaseDecision decision = classify_decrease(last_value_, reading.value, reading.width, config);
    if (!decision.wrap_proven) {
      unproven_decreases_ += 1;
      rebaseline();
      last_event_ = CounterEvent::DecreaseUnproven;
      last_delta_.reset();
      rate_admissible_ = false;
      step.event = last_event_;
      step.reason = reason_for(last_event_);
      step.continuity_intact = false;
      return step;
    }
    delta.delta = decision.delta;
    delta.from_wrap = true;
    wraps_ += 1;
    last_event_ = CounterEvent::Wrap;
  }

  if (spans_gap) {
    gaps_ += 1;
  }
  last_delta_ = delta;
  rate_admissible_ = !spans_gap;
  rebaseline();
  step.event = last_event_;
  step.reason = reason_for(last_event_);
  step.delta = delta;
  step.continuity_intact = true;
  return step;
}

}  // namespace lqf
