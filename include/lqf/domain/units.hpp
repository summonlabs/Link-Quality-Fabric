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

#ifndef LQF_DOMAIN_UNITS_HPP
#define LQF_DOMAIN_UNITS_HPP

#include <string_view>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Explicit units. A gauge without units is not evidence: an unlabelled number
// cannot be compared to a threshold, so it is rejected at the boundary instead
// of being guessed at classification time.
enum class Unit : u8 {
  None = 0,
  DecibelMilliwatt,
  Decibel,
  MilliVolt,
  MilliAmp,
  MicroAmpere,
  PicoSecond,
  NanoSecond,
  MilliSecond,
  Second,
  Count,
  PartsPerMillion,
  Percent,
  Ratio,
  BitsPerSecond,
  Bytes,
  MilliDegreeCelsius,
};

// Dimensional class. Two units are only interchangeable inside one class, and
// even then only with an explicit conversion; this runtime never converts
// silently, it refuses.
enum class UnitClass : u8 {
  Dimensionless = 0,
  Power,
  Ratio,
  Time,
  Count,
  Rate,
  Temperature,
  Current,
};

LQF_API UnitClass unit_class(Unit unit) noexcept;
LQF_API bool units_compatible(Unit lhs, Unit rhs) noexcept;
LQF_API const char* to_string(Unit unit) noexcept;
LQF_API std::string_view describe(Unit unit) noexcept;
LQF_API Status parse_unit(std::string_view text, Unit& out);

}  // namespace lqf

#endif  // LQF_DOMAIN_UNITS_HPP
