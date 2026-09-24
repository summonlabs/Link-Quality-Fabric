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

#ifndef LQF_CLASSIFY_CLASSIFIER_HPP
#define LQF_CLASSIFY_CLASSIFIER_HPP

#include <optional>

#include "lqf/domain/classification.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/runtime/config.hpp"
#include "lqf/store/capability_registry.hpp"
#include "lqf/store/evidence_store.hpp"
#include "lqf/store/link_registry.hpp"

namespace lqf {

// Everything classification needs, borrowed for the duration of a call. No
// classification holds a lock or calls back into the Fabric: the Fabric takes
// its shared lock, builds the report, releases, and only then renders text.
struct ClassifierContext {
  const LinkRegistry* links{nullptr};
  const EvidenceStore* store{nullptr};
  const CapabilityRegistry* capabilities{nullptr};
  const PolicyGenerationRecord* policy{nullptr};
  const FabricLimits* limits{nullptr};
  ClockReading now{};
  FabricEpoch epoch{};

  [[nodiscard]] Status validate() const;
};

// Derives the quality state the evidence actually supports for one link
// generation. Missing evidence is never zero, stale evidence never asserts
// current health, and equal-authority disagreement stays conflicting.
LQF_API LinkQualityReport classify_link(const ClassifierContext& context, const QualityQuery& query);

LQF_API InspectionReport inspect_fabric(const ClassifierContext& context,
                                        const InspectionFilter& filter);

// Freshness of one stored record at one instant. Live records from this process
// incarnation are aged on the steady clock; everything else is never fresh.
LQF_API bool record_is_fresh(const EvidenceRecord& record, i64 validity_nanos,
                            const ClockReading& now, FabricEpoch epoch, i64& age_nanos);

}  // namespace lqf

#endif  // LQF_CLASSIFY_CLASSIFIER_HPP
