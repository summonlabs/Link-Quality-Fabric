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

#include <cstdio>
#include <limits>
#include <sstream>

using namespace lqf;
using namespace lqf::test;

namespace {

// Independent reference model for counter continuity. It is written from the
// specification with explicit modular arithmetic rather than by sharing code
// with the implementation, so agreement is evidence and not a tautology.
struct OracleReading {
  u64 value{0};
  CounterWidth width{CounterWidth::Bits64};
  bool reset{false};
  i64 observed{0};
  u64 sequence{0};
  u64 incarnation{1};
  u64 generation{1};
};

struct OracleStep {
  CounterEvent event{CounterEvent::Baseline};
  bool has_delta{false};
  u64 delta{0};
  i64 elapsed{0};
  bool spans_gap{false};
  bool from_wrap{false};
};

class Oracle {
 public:
  OracleStep step(const OracleReading& reading, const CounterContinuityConfig& config) {
    OracleStep result;
    const bool incarnation_changed = bound_ && reading.incarnation != incarnation_;
    const bool generation_changed = bound_ && reading.generation != generation_;
    if (incarnation_changed || generation_changed) {
      rebaseline(reading);
      result.event = incarnation_changed ? CounterEvent::SourceReincarnated
                                         : CounterEvent::LinkGenerationChanged;
      return result;
    }
    if (reading.width == CounterWidth::Bits32 && reading.value > 0xFFFFFFFFULL) {
      bound_ = false;
      result.event = CounterEvent::OutOfRange;
      return result;
    }
    if (bound_ && reading.width != width_) {
      rebaseline(reading);
      result.event = CounterEvent::WidthChanged;
      return result;
    }
    if (bound_ && reading.sequence < sequence_) {
      result.event = CounterEvent::SequenceRegressed;
      return result;
    }
    if (reading.reset) {
      rebaseline(reading);
      result.event = CounterEvent::Reset;
      return result;
    }
    if (!bound_) {
      rebaseline(reading);
      result.event = CounterEvent::Baseline;
      return result;
    }
    i64 elapsed = reading.observed - observed_;
    if (elapsed < 0) {
      elapsed = 0;
    }
    result.elapsed = elapsed;
    result.spans_gap = elapsed > config.max_gap_nanos || elapsed <= 0;
    if (reading.value >= value_) {
      result.event = CounterEvent::Advance;
      result.has_delta = true;
      result.delta = reading.value - value_;
      rebaseline(reading);
      return result;
    }
    const bool wrap_possible = config.wrap_inference && reading.width != CounterWidth::Unspecified;
    if (!wrap_possible) {
      rebaseline(reading);
      result.event = CounterEvent::DecreaseUnproven;
      return result;
    }
    const unsigned long long modulus =
        reading.width == CounterWidth::Bits32
            ? static_cast<unsigned long long>(1ULL << 32U)
            : std::numeric_limits<unsigned long long>::max();
    const long double ceiling =
        static_cast<long double>(modulus) * static_cast<long double>(config.wrap_ceiling_fraction);
    const long double floor_bound =
        static_cast<long double>(modulus) * static_cast<long double>(config.wrap_floor_fraction);
    const bool above_ceiling = static_cast<long double>(value_) >= ceiling;
    const bool below_floor = static_cast<long double>(reading.value) <= floor_bound;
    if (!above_ceiling || !below_floor) {
      rebaseline(reading);
      result.event = CounterEvent::DecreaseUnproven;
      return result;
    }
    result.event = CounterEvent::Wrap;
    result.has_delta = true;
    result.from_wrap = true;
    result.delta = reading.width == CounterWidth::Bits32
                       ? static_cast<u64>((static_cast<unsigned long long>(1ULL << 32U) - value_) +
                                          reading.value)
                       : reading.value - value_;
    rebaseline(reading);
    return result;
  }

 private:
  void rebaseline(const OracleReading& reading) {
    bound_ = true;
    value_ = reading.value;
    width_ = reading.width;
    observed_ = reading.observed;
    sequence_ = reading.sequence;
    incarnation_ = reading.incarnation;
    generation_ = reading.generation;
  }

  bool bound_{false};
  u64 value_{0};
  CounterWidth width_{CounterWidth::Unspecified};
  i64 observed_{0};
  u64 sequence_{0};
  u64 incarnation_{1};
  u64 generation_{1};
};

}  // namespace

std::string describe_step(CounterEvent event, const std::optional<CounterDelta>& delta) {
  std::ostringstream stream;
  stream << to_string(event);
  if (delta.has_value()) {
    stream << " delta=" << delta->delta << " elapsed=" << delta->elapsed_nanos
           << " gap=" << (delta->spans_gap ? 1 : 0) << " wrap=" << (delta->from_wrap ? 1 : 0);
  } else {
    stream << " no-delta";
  }
  return stream.str();
}

std::string describe_oracle(const OracleStep& step) {
  std::ostringstream stream;
  stream << to_string(step.event);
  if (step.has_delta) {
    stream << " delta=" << step.delta << " elapsed=" << step.elapsed
           << " gap=" << (step.spans_gap ? 1 : 0) << " wrap=" << (step.from_wrap ? 1 : 0);
  } else {
    stream << " no-delta";
  }
  return stream.str();
}

LQF_TEST(counter_property, engine_matches_the_reference_model) {
  const u64 configured = iterations_override();
  const u64 iterations = configured != 0 ? configured : 300;
  const u64 base_seed = seed_override() != 0 ? seed_override() : 0x5EED1234ULL;
  u64 mismatches = 0;
  for (u64 iteration = 0; iteration < iterations; ++iteration) {
    const u64 seed = base_seed + iteration * 0x9E3779B97F4A7C15ULL;
    Rng rng(seed);
    CounterContinuity continuity;
    Oracle oracle;
    CounterContinuityConfig config;
    config.wrap_inference = rng.chance(0.7);
    config.wrap_ceiling_fraction = 0.5 + rng.unit() * 0.4;
    config.wrap_floor_fraction = 0.05 + rng.unit() * 0.3;
    if (config.wrap_floor_fraction >= config.wrap_ceiling_fraction) {
      config.wrap_floor_fraction = config.wrap_ceiling_fraction / 2.0;
    }
    config.max_gap_nanos = static_cast<i64>(rng.range(1, 5)) * 1'000'000'000LL;

    const CounterWidth width = rng.chance(0.5) ? CounterWidth::Bits32 : CounterWidth::Bits64;
    const u64 modulus = width == CounterWidth::Bits32 ? (1ULL << 32U) : 0;
    u64 value = modulus == 0 ? rng.next() : rng.below(modulus);
    i64 observed = 0;
    u64 sequence = 1;
    u64 incarnation = 1;
    u64 generation = 1;

    const u64 steps = 40 + rng.below(60);
    for (u64 index = 0; index < steps; ++index) {
      const double action = rng.unit();
      if (action < 0.08) {
        incarnation += 1;
        sequence = 1;
        value = modulus == 0 ? rng.below(1000) : rng.below(modulus);
      } else if (action < 0.12) {
        generation += 1;
        sequence = 1;
        value = modulus == 0 ? rng.below(1000) : rng.below(modulus);
      } else if (action < 0.30) {
        value = modulus == 0 ? rng.next() : rng.below(modulus);
      } else {
        const u64 advance = rng.below(modulus == 0 ? 1'000'000ULL : modulus / 4 + 1);
        const u64 space =
            modulus == 0 ? std::numeric_limits<u64>::max() - value : modulus - 1 - value;
        value += advance < space ? advance : space;
      }
      observed += static_cast<i64>(rng.range(0, 3)) * 1'000'000'000LL;
      const bool reset = rng.chance(0.05);
      sequence += 1 + rng.below(3);

      CounterObservation observation;
      observation.value = value;
      observation.width = width;
      observation.reset_declared = reset;
      observation.observed_nanos = observed;
      observation.received_steady_nanos = observed;
      observation.sequence = SequenceNumber(sequence);
      observation.incarnation = SourceIncarnation(incarnation);
      observation.link_generation = LinkGeneration(generation);

      OracleReading oracle_reading;
      oracle_reading.value = value;
      oracle_reading.width = width;
      oracle_reading.reset = reset;
      oracle_reading.observed = observed;
      oracle_reading.sequence = sequence;
      oracle_reading.incarnation = incarnation;
      oracle_reading.generation = generation;

      const CounterStep step = continuity.observe(observation, config);
      const OracleStep expected = oracle.step(oracle_reading, config);

      bool agree = step.event == expected.event;
      if (agree && expected.has_delta) {
        agree = step.delta.has_value() && step.delta->delta == expected.delta &&
                step.delta->elapsed_nanos == expected.elapsed &&
                step.delta->spans_gap == expected.spans_gap &&
                step.delta->from_wrap == expected.from_wrap;
      } else if (agree) {
        agree = !step.delta.has_value();
      }
      if (!agree) {
        mismatches += 1;
        if (mismatches <= 3U) {
          std::printf("  seed %s iteration %llu step %llu: engine [%s] reference [%s]\n",
                      text::format_u64(seed).c_str(), static_cast<unsigned long long>(iteration),
                      static_cast<unsigned long long>(index),
                      describe_step(step.event, step.delta).c_str(),
                      describe_oracle(expected).c_str());
          std::printf("  reproduce with: lqf_tests --suite counter_property --seed %s\n",
                      text::format_u64(base_seed).c_str());
        }
      }
    }
  }
  LQF_CHECK_MSG(mismatches == 0, "engine and reference model disagreed " +
                                     text::format_u64(mismatches) + " times (seed " +
                                     text::format_u64(base_seed) + ")");
}

LQF_TEST(counter_property, invariants_hold_for_every_step) {
  const u64 configured = iterations_override();
  const u64 iterations = configured != 0 ? configured : 200;
  const u64 base_seed = (seed_override() != 0 ? seed_override() : 0x5EED1234ULL) ^ 0xA5A5A5A5ULL;
  for (u64 iteration = 0; iteration < iterations; ++iteration) {
    Rng rng(base_seed + iteration * 0xD1B54A32D192ED03ULL);
    CounterContinuity continuity;
    CounterContinuityConfig config;
    config.wrap_inference = true;
    config.max_gap_nanos = 2'000'000'000LL;
    const CounterWidth width = rng.chance(0.5) ? CounterWidth::Bits32 : CounterWidth::Bits64;
    const u64 modulus = width == CounterWidth::Bits32 ? (1ULL << 32U) : 0;
    u64 value = rng.below(1000);
    i64 observed = 0;
    u64 sequence = 1;
    u64 previous_value = value;
    bool have_previous = false;
    for (u64 index = 0; index < 60; ++index) {
      CounterObservation observation;
      observation.value = value;
      observation.width = width;
      observation.observed_nanos = observed;
      observation.received_steady_nanos = observed;
      observation.sequence = SequenceNumber(sequence);
      observation.incarnation = SourceIncarnation(1);
      observation.link_generation = LinkGeneration(1);
      const CounterStep step = continuity.observe(observation, config);

      // A delta exists only for Advance and Wrap. Nothing else may fabricate one.
      bool delta_allowed = false;
      if (step.event == CounterEvent::Advance || step.event == CounterEvent::Wrap) {
        delta_allowed = true;
      }
      LQF_CHECK_EQ(step.delta.has_value(), delta_allowed);
      if (step.event == CounterEvent::Advance && have_previous) {
        LQF_CHECK_EQ(step.delta->delta, value - previous_value);
        LQF_CHECK(!step.delta->from_wrap);
      }
      if (step.event == CounterEvent::Wrap && have_previous) {
        const u64 span =
            modulus == 0 ? value - previous_value : (modulus - previous_value) + value;
        LQF_CHECK_EQ(step.delta->delta, span);
        LQF_CHECK(step.delta->from_wrap);
      }
      if (step.event == CounterEvent::DecreaseUnproven) {
        LQF_CHECK(!step.delta.has_value());
      }
      previous_value = value;
      have_previous = true;

      const double action = rng.unit();
      if (action < 0.2) {
        value = modulus == 0 ? rng.next() : rng.below(modulus);
      } else if (action < 0.3 && modulus != 0) {
        value = modulus - 1 - rng.below(10);
      } else {
        const u64 advance = rng.below(1000);
        const u64 space =
            modulus == 0 ? std::numeric_limits<u64>::max() - value : modulus - 1 - value;
        value += advance < space ? advance : space;
      }
      observed += static_cast<i64>(rng.range(0, 2)) * 1'000'000'000LL;
      sequence += 1;
    }
  }
}
