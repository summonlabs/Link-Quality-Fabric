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

#include "lqf/core/clock.hpp"

#include <chrono>

namespace lqf {
namespace {

i64 to_nanos(std::chrono::nanoseconds value) {
  return static_cast<i64>(value.count());
}

}  // namespace

SystemClock::SystemClock() {
  const auto origin = std::chrono::steady_clock::now().time_since_epoch();
  steady_origin_nanos_ = to_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(origin));
}

ClockReading SystemClock::now() const {
  const auto wall = std::chrono::system_clock::now().time_since_epoch();
  const auto steady = std::chrono::steady_clock::now().time_since_epoch();
  ClockReading reading;
  reading.wall_nanos = to_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(wall));
  reading.steady_nanos =
      to_nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(steady)) - steady_origin_nanos_;
  return reading;
}

ClockReading ManualClock::now() const {
  ClockReading reading;
  reading.wall_nanos = wall_nanos_.load(std::memory_order_relaxed);
  reading.steady_nanos = steady_nanos_.load(std::memory_order_relaxed);
  return reading;
}

void ManualClock::advance_nanos(i64 delta) {
  wall_nanos_.fetch_add(delta, std::memory_order_relaxed);
  steady_nanos_.fetch_add(delta, std::memory_order_relaxed);
}

std::shared_ptr<Clock> make_system_clock() { return std::make_shared<SystemClock>(); }

}  // namespace lqf
