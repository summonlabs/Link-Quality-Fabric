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

#ifndef LQF_CORE_STATUS_HPP
#define LQF_CORE_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "lqf/core/assert.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Typed outcome classification. UNKNOWN, UNSUPPORTED, STALE, CONFLICTING,
// INCOMPLETE, REFUSED and INVALID are never collapsed into success: every
// fallible entry point returns a Status whose code is one of these.
enum class StatusCode : std::uint8_t {
  Ok = 0,
  Invalid,        // caller supplied a malformed value
  NotFound,       // the requested identity is not known here
  Unsupported,    // recognised but deliberately not implemented or not declared
  Conflict,       // equal-authority disagreement that must not be averaged
  Stale,          // evidence or authority is too old to act on
  Fenced,         // superseded generation, incarnation or epoch
  Rejected,       // policy or validation refused the operation
  Duplicate,      // already-seen identity with an identical payload
  IdMismatch,     // already-seen identity with a different payload
  Overflow,       // checked arithmetic refused to wrap
  LimitExceeded,  // a configured bound would be exceeded
  Corrupt,        // integrity check failed
  Unavailable,    // the resource is not present or not open
  Busy,           // the resource cannot accept work right now
  Cancelled,      // shutdown or cancellation interrupted the operation
  Refused,        // the operation is disabled by configuration
  Protocol,       // peer framing or message was invalid
  Io,             // operating system level failure
  Internal,       // invariant violation inside the runtime
};

LQF_API const char* to_string(StatusCode code) noexcept;

// A status is a value, not an exception. It is cheap to copy and never throws.
class LQF_API Status {
 public:
  Status() noexcept = default;

  // Named success() rather than ok() because ok() is the predicate.
  static Status success() noexcept { return Status{}; }

  static Status error(StatusCode code, std::string message);

  LQF_NODISCARD bool ok() const noexcept { return code_ == StatusCode::Ok; }
  LQF_NODISCARD explicit operator bool() const noexcept { return ok(); }
  LQF_NODISCARD StatusCode code() const noexcept { return code_; }
  LQF_NODISCARD const std::string& message() const noexcept { return message_; }
  LQF_NODISCARD std::string to_text() const;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_;
  }
  friend bool operator!=(const Status& lhs, const Status& rhs) noexcept { return !(lhs == rhs); }

 private:
  StatusCode code_{StatusCode::Ok};
  std::string message_{};
};

// Result of an operation that yields a value. Accessing the value of a failed
// outcome is a programming error and is trapped in debug builds.
template <class T>
class Outcome {
 public:
  Outcome(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Outcome(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  LQF_NODISCARD bool ok() const noexcept { return status_.ok(); }
  LQF_NODISCARD const Status& status() const noexcept { return status_; }

  LQF_NODISCARD T& value() noexcept {
    LQF_ASSERT_MSG(ok(), "Outcome::value() on a failed outcome");
    return *value_;
  }
  LQF_NODISCARD const T& value() const noexcept {
    LQF_ASSERT_MSG(ok(), "Outcome::value() on a failed outcome");
    return *value_;
  }
  LQF_NODISCARD T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }
  LQF_NODISCARD const T* operator->() const noexcept { return value_ ? &*value_ : nullptr; }
  LQF_NODISCARD T* operator->() noexcept { return value_ ? &*value_ : nullptr; }

 private:
  std::optional<T> value_{};
  Status status_{};
};

}  // namespace lqf

#endif  // LQF_CORE_STATUS_HPP
