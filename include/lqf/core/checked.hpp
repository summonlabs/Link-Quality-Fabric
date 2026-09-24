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

#ifndef LQF_CORE_CHECKED_HPP
#define LQF_CORE_CHECKED_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lqf {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// Checked arithmetic for every value derived from outside this process.
// Nothing that arrives over the wire, from a file or from a caller is ever
// allowed to overflow, wrap or silently truncate into a size.
[[nodiscard]] constexpr bool checked_add_u64(u64 lhs, u64 rhs, u64& out) noexcept {
  if (lhs > std::numeric_limits<u64>::max() - rhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

[[nodiscard]] constexpr bool checked_mul_u64(u64 lhs, u64 rhs, u64& out) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<u64>::max() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

[[nodiscard]] constexpr bool checked_add_i64(i64 lhs, i64 rhs, i64& out) noexcept {
  if ((rhs > 0 && lhs > std::numeric_limits<i64>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<i64>::min() - rhs)) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

[[nodiscard]] constexpr bool checked_sub_i64(i64 lhs, i64 rhs, i64& out) noexcept {
  if (rhs == std::numeric_limits<i64>::min()) {
    return false;
  }
  return checked_add_i64(lhs, -rhs, out);
}

[[nodiscard]] constexpr bool checked_mul_size(std::size_t lhs, std::size_t rhs,
                                              std::size_t& out) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

[[nodiscard]] constexpr bool checked_add_size(std::size_t lhs, std::size_t rhs,
                                              std::size_t& out) noexcept {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

// True when an unsigned 64-bit count can be represented by std::size_t and by
// the element type used for indexing.
template <class To>
[[nodiscard]] constexpr bool fits_in(u64 value) noexcept {
  static_assert(std::is_unsigned_v<To>, "fits_in expects an unsigned destination type");
  return value <= static_cast<u64>(std::numeric_limits<To>::max());
}

[[nodiscard]] constexpr bool is_finite(double value) noexcept {
  return value == value && value != std::numeric_limits<double>::infinity() &&
         value != -std::numeric_limits<double>::infinity();
}

// Rejects NaN, infinities and negative values where a non-negative magnitude is
// required. Extreme but finite magnitudes (1e308, denormals) are accepted: they
// are real measurements and classifications must handle them.
[[nodiscard]] constexpr bool is_valid_magnitude(double value) noexcept {
  return is_finite(value) && value >= 0.0;
}

}  // namespace lqf

#endif  // LQF_CORE_CHECKED_HPP
