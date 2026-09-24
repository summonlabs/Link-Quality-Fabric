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

#ifndef LQF_CORE_STRONG_HPP
#define LQF_CORE_STRONG_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/export.hpp"

namespace lqf {

// ---------------------------------------------------------------------------
// Strongly typed identities. A LinkId can never be silently used where a
// SourceId is expected, and a generation can never be used where an
// incarnation is expected.
// ---------------------------------------------------------------------------

template <class Tag, class T>
class StrongValue {
 public:
  using tag_type = Tag;
  using value_type = T;

  constexpr StrongValue() = default;
  constexpr explicit StrongValue(T value) : value_(std::move(value)) {}

  [[nodiscard]] constexpr const T& value() const noexcept { return value_; }

  [[nodiscard]] constexpr bool is_empty() const noexcept {
    if constexpr (std::is_arithmetic_v<T>) {
      return value_ == T{};
    } else {
      return value_.empty();
    }
  }

  friend constexpr bool operator==(const StrongValue&, const StrongValue&) = default;
  friend constexpr auto operator<=>(const StrongValue&, const StrongValue&) = default;

 private:
  T value_{};
};

// Monotonic 64-bit identity used for generations, incarnations, epochs and
// sequence numbers. Distinct tags make them mutually incompatible.
template <class Tag>
class StrongCounter {
 public:
  using tag_type = Tag;

  constexpr StrongCounter() = default;
  constexpr explicit StrongCounter(u64 value) : value_(value) {}

  [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr StrongCounter next() const noexcept {
    return StrongCounter(value_ + 1U);
  }
  [[nodiscard]] constexpr StrongCounter prev() const noexcept {
    return StrongCounter(value_ == 0 ? 0 : value_ - 1U);
  }

  friend constexpr bool operator==(StrongCounter, StrongCounter) = default;
  friend constexpr auto operator<=>(StrongCounter, StrongCounter) = default;

 private:
  u64 value_{0};
};

struct LinkIdTag {};
struct SourceIdTag {};
struct PolicyIdTag {};
struct RuleIdTag {};
struct LaneIdTag {};
struct LinkGenerationTag {};
struct SourceIncarnationTag {};
struct PolicyGenerationTag {};
struct FabricEpochTag {};
struct SequenceNumberTag {};
struct CapabilityRevisionTag {};
struct IngestOrdinalTag {};
struct AuthorityRankTag {};

using LinkId = StrongValue<LinkIdTag, std::string>;
using SourceId = StrongValue<SourceIdTag, std::string>;
using PolicyId = StrongValue<PolicyIdTag, std::string>;
using RuleId = StrongValue<RuleIdTag, std::string>;
using LaneId = StrongValue<LaneIdTag, u32>;

using LinkGeneration = StrongCounter<LinkGenerationTag>;
using SourceIncarnation = StrongCounter<SourceIncarnationTag>;
using PolicyGeneration = StrongCounter<PolicyGenerationTag>;
using FabricEpoch = StrongCounter<FabricEpochTag>;
using SequenceNumber = StrongCounter<SequenceNumberTag>;
using CapabilityRevision = StrongCounter<CapabilityRevisionTag>;
using IngestOrdinal = StrongCounter<IngestOrdinalTag>;
using AuthorityRank = StrongCounter<AuthorityRankTag>;

// Identifier text is validated once, at the boundary, against a documented
// grammar. Rejecting early keeps every downstream comparison cheap and total.
inline constexpr std::size_t kMaxIdentityBytes = 128;
inline constexpr std::size_t kMaxOriginBytes = 96;

LQF_API Status validate_identity_text(std::string_view value, std::size_t max_bytes,
                                      const char* what);
LQF_API Status validate_lane_id(std::uint32_t lane, std::uint32_t max_lanes);

// Missing lane means "the link-wide aggregate", which is a meaningful and
// distinct dimension from lane 0.
struct LaneDimension {
  bool aggregate{true};
  LaneId lane{};

  LaneDimension() = default;
  explicit LaneDimension(LaneId value) : aggregate(false), lane(value) {}

  [[nodiscard]] static LaneDimension aggregate_dimension() { return LaneDimension{}; }
  [[nodiscard]] bool is_aggregate() const noexcept { return aggregate; }
  [[nodiscard]] const LaneId& value() const noexcept { return lane; }

  friend bool operator==(const LaneDimension&, const LaneDimension&) = default;
  friend auto operator<=>(const LaneDimension&, const LaneDimension&) = default;
};

}  // namespace lqf

namespace std {

template <class Tag, class T>
struct hash<lqf::StrongValue<Tag, T>> {
  size_t operator()(const lqf::StrongValue<Tag, T>& id) const noexcept {
    return std::hash<T>{}(id.value());
  }
};

template <class Tag>
struct hash<lqf::StrongCounter<Tag>> {
  size_t operator()(const lqf::StrongCounter<Tag>& counter) const noexcept {
    return std::hash<lqf::u64>{}(counter.value());
  }
};

}  // namespace std

#endif  // LQF_CORE_STRONG_HPP
