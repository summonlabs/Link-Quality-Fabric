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

#include "lqf/core/text.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace lqf::text {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool is_space(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\v' ||
         value == '\f';
}

}  // namespace

std::string to_lower_ascii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    if (character >= 'A' && character <= 'Z') {
      out.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      out.push_back(character);
    }
  }
  return out;
}

std::string_view trim_ascii(std::string_view value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && is_space(value[begin])) {
    ++begin;
  }
  while (end > begin && is_space(value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool equals_ascii_ci(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    char left = lhs[index];
    char right = rhs[index];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t position = value.find(delimiter, start);
    if (position == std::string_view::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, position - start));
    start = position + 1;
  }
  return parts;
}

std::string format_u64(u64 value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

std::string format_i64(i64 value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

std::string format_double(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  std::string out(buffer, static_cast<std::size_t>(result.ptr - buffer));
  // A double that renders as an integer would otherwise be ambiguous with a
  // counter reading in rendered evidence. Keep the decimal point explicit.
  if (out.find('.') == std::string::npos && out.find('e') == std::string::npos &&
      out.find('E') == std::string::npos && out.find("inf") == std::string::npos &&
      out.find("nan") == std::string::npos) {
    out.append(".0");
  }
  return out;
}

std::string hex_u64(u64 value, int min_digits) {
  char buffer[16];
  int digits = 0;
  if (value == 0) {
    buffer[digits++] = '0';
  }
  while (value != 0 && digits < 16) {
    buffer[digits++] = kHexDigits[value & 0xFU];
    value >>= 4U;
  }
  while (digits < min_digits && digits < 16) {
    buffer[digits++] = '0';
  }
  std::string out;
  out.reserve(static_cast<std::size_t>(digits));
  for (int index = digits - 1; index >= 0; --index) {
    out.push_back(buffer[index]);
  }
  return out;
}

std::string hex_bytes(const unsigned char* data, std::size_t size) {
  std::string out;
  out.reserve(size * 2U);
  for (std::size_t index = 0; index < size; ++index) {
    out.push_back(kHexDigits[(data[index] >> 4U) & 0xFU]);
    out.push_back(kHexDigits[data[index] & 0xFU]);
  }
  return out;
}

Status parse_u64(std::string_view value, u64& out) {
  const std::string_view trimmed = trim_ascii(value);
  if (trimmed.empty()) {
    return Status::error(StatusCode::Invalid, "empty unsigned integer");
  }
  u64 parsed = 0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
    return Status::error(StatusCode::Invalid, "malformed unsigned integer");
  }
  out = parsed;
  return Status::success();
}

Status parse_i64(std::string_view value, i64& out) {
  const std::string_view trimmed = trim_ascii(value);
  if (trimmed.empty()) {
    return Status::error(StatusCode::Invalid, "empty integer");
  }
  i64 parsed = 0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
    return Status::error(StatusCode::Invalid, "malformed integer");
  }
  out = parsed;
  return Status::success();
}

Status parse_double(std::string_view value, double& out) {
  const std::string_view trimmed = trim_ascii(value);
  if (trimmed.empty()) {
    return Status::error(StatusCode::Invalid, "empty number");
  }
  double parsed = 0.0;
  const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
    return Status::error(StatusCode::Invalid, "malformed number");
  }
  out = parsed;
  return Status::success();
}

Status parse_bool(std::string_view value, bool& out) {
  const std::string_view trimmed = trim_ascii(value);
  if (equals_ascii_ci(trimmed, "true") || trimmed == "1") {
    out = true;
    return Status::success();
  }
  if (equals_ascii_ci(trimmed, "false") || trimmed == "0") {
    out = false;
    return Status::success();
  }
  return Status::error(StatusCode::Invalid, "malformed boolean");
}

}  // namespace lqf::text
