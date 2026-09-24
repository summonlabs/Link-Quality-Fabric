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

#ifndef LQF_DOMAIN_METRIC_HPP
#define LQF_DOMAIN_METRIC_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/domain/units.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Metric families are transport agnostic on purpose. Whether a level was
// produced by an optical transceiver, an electrical SerDes or a synthetic
// generator, the family and units are the same; only the provenance differs.
enum class MetricFamily : u8 {
  Unspecified = 0,
  SignalPower,    // received or transmitted level (dBm, mV, ...)
  SignalRatio,    // signal to noise ratio, margin, extinction ratio (dB)
  ErrorCounter,   // cumulative error events of any layer
  LossRatio,      // gauge loss in ppm/percent
  Timing,         // latency, jitter, skew (ns)
  Throughput,     // cumulative volume or a rate
  Environmental,  // temperature and similar
  Bias,           // bias current or voltage of a transducer
  LinkStateHint,  // opaque transport-provided hint, consumed as evidence only
  Count,
};

LQF_API const char* to_string(MetricFamily family) noexcept;
LQF_API std::string_view describe(MetricFamily family) noexcept;
LQF_API Status parse_metric_family(std::string_view text, MetricFamily& out);

// Sample semantics decide which derivations are legal. A gauge is a value with
// a validity window. A counter is a cumulative reading whose only meaningful
// derivations are differences, and only when continuity is proven.
enum class SampleSemantics : u8 { Gauge = 0, Counter };

LQF_API const char* to_string(SampleSemantics semantics) noexcept;
LQF_API Status parse_sample_semantics(std::string_view text, SampleSemantics& out);

// Counter width. Unspecified is a real, load-bearing declaration: without a
// declared modulus no wrap can be proven, so a decrease is conservatively a
// reset and no delta is fabricated.
enum class CounterWidth : u8 { Unspecified = 0, Bits32, Bits64 };

LQF_API const char* to_string(CounterWidth width) noexcept;
LQF_API std::optional<u64> counter_modulus(CounterWidth width) noexcept;
LQF_API bool counter_value_in_range(u64 value, CounterWidth width) noexcept;
LQF_API Status parse_counter_width(std::string_view text, CounterWidth& out);

// What a threshold rule classifies.
enum class MetricBasis : u8 { GaugeValue = 0, CounterRatePerSecond, CounterDelta };

LQF_API const char* to_string(MetricBasis basis) noexcept;
LQF_API Status parse_metric_basis(std::string_view text, MetricBasis& out);

// A metric identity is a family plus a canonical lower-case dotted name. The
// name space is open so that transports can be described without pretending
// vendor specific values are portable.
struct MetricId {
  MetricFamily family{MetricFamily::Unspecified};
  std::string name{};

  MetricId() = default;
  MetricId(MetricFamily family_in, std::string name_in)
      : family(family_in), name(std::move(name_in)) {}

  [[nodiscard]] bool empty() const noexcept {
    return family == MetricFamily::Unspecified && name.empty();
  }

  friend bool operator==(const MetricId&, const MetricId&) = default;
  friend auto operator<=>(const MetricId&, const MetricId&) = default;
};

inline constexpr std::size_t kMaxMetricNameBytes = 96;

LQF_API Status validate_metric_id(const MetricId& id);
LQF_API std::string render_metric_id(const MetricId& id);

struct MetricDescriptor {
  MetricId id{};
  Unit unit{Unit::None};
  SampleSemantics semantics{SampleSemantics::Gauge};
  MetricBasis basis{MetricBasis::GaugeValue};
  CounterWidth counter_width{CounterWidth::Unspecified};
  bool supports_lanes{false};
  std::string description{};
};

// The catalog is the registry of metric identities this runtime understands.
// Registering is explicit; an unknown metric is rejected rather than stored
// under a guessed family.
class LQF_API MetricCatalog {
 public:
  explicit MetricCatalog(std::size_t max_descriptors = 256);

  Status register_descriptor(MetricDescriptor descriptor);
  [[nodiscard]] const MetricDescriptor* find(const MetricId& id) const;
  [[nodiscard]] bool contains(const MetricId& id) const;
  [[nodiscard]] std::vector<MetricDescriptor> snapshot() const;
  [[nodiscard]] std::size_t size() const noexcept { return descriptors_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return max_descriptors_; }

  // Vendor neutral metric identities drawn from the families above. No
  // vendor-specific signal value is present anywhere in this catalog.
  static MetricCatalog with_builtin_descriptors(std::size_t max_descriptors = 256);

 private:
  std::vector<MetricDescriptor> descriptors_{};
  std::size_t max_descriptors_{256};
};

}  // namespace lqf

#endif  // LQF_DOMAIN_METRIC_HPP
