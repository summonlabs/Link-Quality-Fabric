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

#include "lqf/domain/units.hpp"

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct UnitInfo {
  Unit unit;
  UnitClass unit_class;
  const char* name;
  const char* description;
};

constexpr UnitInfo kUnits[] = {
    {Unit::None, UnitClass::Dimensionless, "none", "no unit declared"},
    {Unit::DecibelMilliwatt, UnitClass::Power, "dBm", "power relative to one milliwatt"},
    {Unit::Decibel, UnitClass::Ratio, "dB", "logarithmic ratio"},
    {Unit::MilliVolt, UnitClass::Power, "mV", "electrical level"},
    {Unit::MilliAmp, UnitClass::Current, "mA", "current"},
    {Unit::MicroAmpere, UnitClass::Current, "uA", "current"},
    {Unit::PicoSecond, UnitClass::Time, "ps", "time"},
    {Unit::NanoSecond, UnitClass::Time, "ns", "time"},
    {Unit::MilliSecond, UnitClass::Time, "ms", "time"},
    {Unit::Second, UnitClass::Time, "s", "time"},
    {Unit::Count, UnitClass::Count, "count", "dimensionless event count"},
    {Unit::PartsPerMillion, UnitClass::Ratio, "ppm", "ratio in parts per million"},
    {Unit::Percent, UnitClass::Ratio, "percent", "ratio in percent"},
    {Unit::Ratio, UnitClass::Ratio, "ratio", "dimensionless ratio"},
    {Unit::BitsPerSecond, UnitClass::Rate, "bit/s", "rate"},
    {Unit::Bytes, UnitClass::Count, "bytes", "byte volume"},
    {Unit::MilliDegreeCelsius, UnitClass::Temperature, "mC", "temperature"},
};

const UnitInfo* lookup(Unit unit) {
  for (const UnitInfo& info : kUnits) {
    if (info.unit == unit) {
      return &info;
    }
  }
  return nullptr;
}

}  // namespace

UnitClass unit_class(Unit unit) noexcept {
  const UnitInfo* info = lookup(unit);
  return info == nullptr ? UnitClass::Dimensionless : info->unit_class;
}

bool units_compatible(Unit lhs, Unit rhs) noexcept {
  if (lhs == Unit::None || rhs == Unit::None) {
    return false;
  }
  return unit_class(lhs) == unit_class(rhs);
}

const char* to_string(Unit unit) noexcept {
  const UnitInfo* info = lookup(unit);
  return info == nullptr ? "invalid" : info->name;
}

std::string_view describe(Unit unit) noexcept {
  const UnitInfo* info = lookup(unit);
  return info == nullptr ? std::string_view("unknown unit") : std::string_view(info->description);
}

Status parse_unit(std::string_view text_value, Unit& out) {
  const std::string trimmed = text::to_lower_ascii(text::trim_ascii(text_value));
  for (const UnitInfo& info : kUnits) {
    if (trimmed == text::to_lower_ascii(info.name)) {
      out = info.unit;
      return Status::success();
    }
  }
  return Status::error(StatusCode::Invalid, "unknown unit: " + std::string(text_value));
}

}  // namespace lqf
