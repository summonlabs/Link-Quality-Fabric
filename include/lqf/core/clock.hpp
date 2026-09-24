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

#ifndef LQF_CORE_CLOCK_HPP
#define LQF_CORE_CLOCK_HPP

#include <atomic>
#include <memory>
#include <string>

#include "lqf/core/checked.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Two readings of the same instant:
//   wall_nanos   - UTC since the Unix epoch, persisted with every record so a
//                  later process can report an honest age;
//   steady_nanos - monotonic, process relative, never persisted for arithmetic.
// Freshness inside a running process is computed from steady_nanos only, which
// makes it immune to wall clock steps. Recovered evidence is never fresh, so it
// never depends on either reading.
struct ClockReading {
  i64 wall_nanos{0};
  i64 steady_nanos{0};
};

class LQF_API Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  [[nodiscard]] virtual ClockReading now() const = 0;
  [[nodiscard]] virtual const char* name() const noexcept = 0;
};

// Real time: system_clock for wall and steady_clock for monotonic.
class LQF_API SystemClock final : public Clock {
 public:
  SystemClock();
  [[nodiscard]] ClockReading now() const override;
  [[nodiscard]] const char* name() const noexcept override { return "system"; }

 private:
  i64 steady_origin_nanos_{0};
};

// Deterministic time for tests and replay. Thread safe: advancing and reading
// may race, and a reading is always a single consistent point.
class LQF_API ManualClock final : public Clock {
 public:
  explicit ManualClock(i64 wall_nanos = 1'700'000'000'000'000'000LL, i64 steady_nanos = 0)
      : wall_nanos_(wall_nanos), steady_nanos_(steady_nanos) {}

  [[nodiscard]] ClockReading now() const override;
  [[nodiscard]] const char* name() const noexcept override { return "manual"; }

  void advance_nanos(i64 delta);
  void advance_millis(i64 delta) { advance_nanos(delta * 1'000'000LL); }
  void advance_seconds(i64 delta) { advance_nanos(delta * 1'000'000'000LL); }
  void set_wall_nanos(i64 wall_nanos) { wall_nanos_.store(wall_nanos); }
  void set_steady_nanos(i64 steady_nanos) { steady_nanos_.store(steady_nanos); }

 private:
  std::atomic<i64> wall_nanos_{0};
  std::atomic<i64> steady_nanos_{0};
};

LQF_API std::shared_ptr<Clock> make_system_clock();

}  // namespace lqf

#endif  // LQF_CORE_CLOCK_HPP
