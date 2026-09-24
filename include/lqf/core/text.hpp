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

#ifndef LQF_CORE_TEXT_HPP
#define LQF_CORE_TEXT_HPP

#include <string>
#include <string_view>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/export.hpp"

namespace lqf::text {

LQF_API std::string to_lower_ascii(std::string_view value);
LQF_API std::string_view trim_ascii(std::string_view value);
LQF_API bool equals_ascii_ci(std::string_view lhs, std::string_view rhs);
LQF_API std::vector<std::string_view> split(std::string_view value, char delimiter);

// Deterministic, locale-independent numeric rendering. Every digest, every
// explanation and every persisted corpus depends on these being stable across
// processes, locales and runs.
LQF_API std::string format_u64(u64 value);
LQF_API std::string format_i64(i64 value);
LQF_API std::string format_double(double value);
LQF_API std::string hex_u64(u64 value, int min_digits = 0);
LQF_API std::string hex_bytes(const unsigned char* data, std::size_t size);

LQF_API Status parse_u64(std::string_view value, u64& out);
LQF_API Status parse_i64(std::string_view value, i64& out);
LQF_API Status parse_double(std::string_view value, double& out);
LQF_API Status parse_bool(std::string_view value, bool& out);

template <class T>
[[nodiscard]] std::string to_text(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, float>) {
    return format_double(static_cast<double>(value));
  } else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>) {
    return format_i64(static_cast<i64>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return format_u64(static_cast<u64>(value));
  } else {
    return value.to_text();
  }
}

template <class Tag, class T>
[[nodiscard]] std::string to_text(const StrongValue<Tag, T>& value) {
  return to_text(value.value());
}

template <class Tag>
[[nodiscard]] std::string to_text(const StrongCounter<Tag>& value) {
  return format_u64(value.value());
}

}  // namespace lqf::text

#endif  // LQF_CORE_TEXT_HPP
