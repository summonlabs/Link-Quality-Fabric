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

#ifndef LQF_DOMAIN_CAPABILITY_HPP
#define LQF_DOMAIN_CAPABILITY_HPP

#include <optional>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/domain/identity.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/export.hpp"

namespace lqf {

// A declaration of what a source actually measures. This is the only thing that
// makes a metric family SUPPORTED for a link. Metrics that are not declared
// stay UNSUPPORTED, and a family a source explicitly declares as not measured
// is reported as declared-unsupported rather than missing.
struct MetricCapability {
  MetricId metric{};
  Unit unit{Unit::None};
  SampleSemantics semantics{SampleSemantics::Gauge};
  CounterWidth counter_width{CounterWidth::Unspecified};
  bool lanes_declared{false};
  u32 lane_count{0};
  // Declared validity window for gauge readings of this metric. Zero means the
  // policy default applies.
  i64 validity_nanos{0};

  friend bool operator==(const MetricCapability&, const MetricCapability&) = default;
};

struct CapabilityDeclaration {
  SourceIdentity source{};
  // Unscoped declarations apply to every link this source feeds; scoped ones
  // apply to exactly one link generation.
  std::optional<LinkIdentity> link_scope{};
  CapabilityRevision revision{CapabilityRevision(1)};
  TransportKind transport{TransportKind::Unspecified};
  EvidenceClass evidence_class{EvidenceClass::Real};
  Timestamp declared_at{};
  std::vector<MetricCapability> metrics{};
  std::vector<MetricId> unsupported_metrics{};
  std::string note{};
};

LQF_API Status validate_capability(const CapabilityDeclaration& declaration,
                                  const MetricCatalog& catalog,
                                  const ObservationLimits& limits,
                                  std::size_t max_metrics);

// Which sources back a metric, and which explicitly do not.
struct DeclaredSource {
  SourceIdentity source{};
  CapabilityRevision revision{};
  EvidenceClass evidence_class{EvidenceClass::Real};
  TransportKind transport{TransportKind::Unspecified};
  bool lanes_declared{false};
  u32 lane_count{0};
  Unit unit{Unit::None};
  SampleSemantics semantics{SampleSemantics::Gauge};
  CounterWidth counter_width{CounterWidth::Unspecified};
  i64 validity_nanos{0};
};

struct MetricCapabilityView {
  MetricId metric{};
  std::vector<DeclaredSource> declared_by{};
  std::vector<DeclaredSource> declared_unsupported_by{};

  [[nodiscard]] bool supported() const noexcept { return !declared_by.empty(); }
  [[nodiscard]] bool explicitly_unsupported() const noexcept {
    return declared_by.empty() && !declared_unsupported_by.empty();
  }
};

}  // namespace lqf

#endif  // LQF_DOMAIN_CAPABILITY_HPP
