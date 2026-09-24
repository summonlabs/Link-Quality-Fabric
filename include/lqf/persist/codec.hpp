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

#ifndef LQF_PERSIST_CODEC_HPP
#define LQF_PERSIST_CODEC_HPP

#include <array>
#include <string>
#include <vector>

#include "lqf/domain/capability.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/domain/policy.hpp"
#include "lqf/persist/archive.hpp"
#include "lqf/runtime/config.hpp"
#include "lqf/transport/frame.hpp"

namespace lqf {

#define LQF_ENUM_BOUND(type, maximum)         \
  template <>                                 \
  struct enum_bound<type> {                   \
    static constexpr bool known = true;       \
    static constexpr u64 max_value = maximum; \
  }

LQF_ENUM_BOUND(MetricFamily, static_cast<u64>(MetricFamily::Count) - 1U);
LQF_ENUM_BOUND(TransportKind, static_cast<u64>(TransportKind::Count) - 1U);
LQF_ENUM_BOUND(EvidenceClass, static_cast<u64>(EvidenceClass::Count) - 1U);
LQF_ENUM_BOUND(EvidenceOrigin, static_cast<u64>(EvidenceOrigin::Count) - 1U);
LQF_ENUM_BOUND(CounterEvent, static_cast<u64>(CounterEvent::Count) - 1U);
LQF_ENUM_BOUND(ReasonCode, static_cast<u64>(ReasonCode::Count) - 1U);
LQF_ENUM_BOUND(MessageType, static_cast<u64>(MessageType::Error));
LQF_ENUM_BOUND(StatusCode, static_cast<u64>(StatusCode::Internal));
LQF_ENUM_BOUND(QualityState, static_cast<u64>(QualityState::Conflicting));
LQF_ENUM_BOUND(ConfidenceLevel, static_cast<u64>(ConfidenceLevel::High));
LQF_ENUM_BOUND(WindowAxis, static_cast<u64>(WindowAxis::ReceiveTime));
LQF_ENUM_BOUND(SampleSemantics, static_cast<u64>(SampleSemantics::Counter));
LQF_ENUM_BOUND(CounterWidth, static_cast<u64>(CounterWidth::Bits64));
LQF_ENUM_BOUND(MetricBasis, static_cast<u64>(MetricBasis::CounterDelta));
LQF_ENUM_BOUND(Unit, static_cast<u64>(Unit::MilliDegreeCelsius));
LQF_ENUM_BOUND(UnitClass, static_cast<u64>(UnitClass::Current));

#undef LQF_ENUM_BOUND

// Field visitation for every persisted and every transported type. Both
// directions use the same description, which is what makes an encoder and a
// decoder drift impossible rather than merely unlikely.
template <class Archive>
void visit_fields(Archive& archive, LinkIdentity& value) {
  archive.field(value.id);
  archive.field(value.generation);
}

template <class Archive>
void visit_fields(Archive& archive, SourceIdentity& value) {
  archive.field(value.id);
  archive.field(value.incarnation);
}

template <class Archive>
void visit_fields(Archive& archive, LaneDimension& value) {
  archive.field(value.aggregate);
  archive.field(value.lane);
}

template <class Archive>
void visit_fields(Archive& archive, Provenance& value) {
  archive.field(value.transport);
  archive.field(value.evidence_class);
  archive.field(value.origin);
  archive.field(value.producer);
  archive.field(value.clock_synchronized);
  archive.field(value.declared_clock_offset_nanos);
}

template <class Archive>
void visit_fields(Archive& archive, MetricId& value) {
  archive.field(value.family);
  archive.field(value.name);
}

template <class Archive>
void visit_fields(Archive& archive, Timestamp& value) {
  archive.field(value.unix_nanos);
  archive.field(value.synchronized);
}

template <class Archive>
void visit_fields(Archive& archive, ReceiveStamp& value) {
  archive.field(value.wall_nanos);
  archive.field(value.steady_nanos);
  archive.field(value.epoch);
}

template <class Archive>
void visit_fields(Archive& archive, GaugeReading& value) {
  archive.field(value.value);
  archive.field(value.validity_nanos);
}

template <class Archive>
void visit_fields(Archive& archive, CounterReading& value) {
  archive.field(value.value);
  archive.field(value.width);
  archive.field(value.reset_declared);
}

template <class Archive>
void visit_fields(Archive& archive, Observation& value) {
  archive.field(value.link);
  archive.field(value.source);
  archive.field(value.sequence);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.unit);
  archive.field(value.reading);
  archive.field(value.observed_at);
  archive.field(value.interval_nanos);
  archive.field(value.authority);
  archive.field(value.provenance);
}

template <class Archive>
void visit_fields(Archive& archive, EvidenceRecord& value) {
  archive.field(value.observation);
  archive.field(value.received);
  archive.field(value.ordinal);
  archive.field(value.origin);
}

template <class Archive>
void visit_fields(Archive& archive, MetricCapability& value) {
  archive.field(value.metric);
  archive.field(value.unit);
  archive.field(value.semantics);
  archive.field(value.counter_width);
  archive.field(value.lanes_declared);
  archive.field(value.lane_count);
  archive.field(value.validity_nanos);
}

template <class Archive>
void visit_fields(Archive& archive, CapabilityDeclaration& value) {
  archive.field(value.source);
  archive.field(value.link_scope);
  archive.field(value.revision);
  archive.field(value.transport);
  archive.field(value.evidence_class);
  archive.field(value.declared_at);
  archive.field(value.metrics);
  archive.field(value.unsupported_metrics);
  archive.field(value.note);
}

template <class Archive>
void visit_fields(Archive& archive, DeclaredSource& value) {
  archive.field(value.source);
  archive.field(value.revision);
  archive.field(value.evidence_class);
  archive.field(value.transport);
  archive.field(value.lanes_declared);
  archive.field(value.lane_count);
  archive.field(value.unit);
  archive.field(value.semantics);
  archive.field(value.counter_width);
  archive.field(value.validity_nanos);
}

template <class Archive>
void visit_fields(Archive& archive, MetricCapabilityView& value) {
  archive.field(value.metric);
  archive.field(value.declared_by);
  archive.field(value.declared_unsupported_by);
}

template <class Archive>
void visit_fields(Archive& archive, ThresholdBand& value) {
  archive.field(value.id);
  archive.field(value.lower);
  archive.field(value.upper);
  archive.field(value.state);
  archive.field(value.label);
}

template <class Archive>
void visit_fields(Archive& archive, MetricRule& value) {
  archive.field(value.metric);
  archive.field(value.basis);
  archive.field(value.bands);
  archive.field(value.conflict_tolerance);
  archive.field(value.validity_nanos);
  archive.field(value.max_continuity_gap_nanos);
  archive.field(value.wrap_inference);
  archive.field(value.wrap_ceiling_fraction);
  archive.field(value.wrap_floor_fraction);
}

template <class Archive>
void visit_fields(Archive& archive, FreshnessPolicy& value) {
  archive.field(value.default_validity_nanos);
  archive.field(value.family_validity_nanos);
  archive.field(value.default_continuity_gap_nanos);
  archive.field(value.min_rate_interval_nanos);
}

template <class Archive>
void visit_fields(Archive& archive, SeverityTable& value) {
  for (auto& entry : value.rank) {
    archive.field(entry);
  }
}

template <class Archive>
void visit_fields(Archive& archive, AggregationPolicy& value) {
  archive.field(value.severity);
  archive.field(value.conflicting_dominates);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyDocument& value) {
  archive.field(value.id);
  archive.field(value.rules);
  archive.field(value.freshness);
  archive.field(value.aggregation);
  archive.field(value.unsupported_metrics);
  archive.field(value.require_capability);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyStamp& value) {
  archive.field(value.id);
  archive.field(value.generation);
  archive.field(value.content_hash);
}

template <class Archive>
void visit_fields(Archive& archive, PolicyGenerationRecord& value) {
  archive.field(value.stamp);
  archive.field(value.published_at);
  archive.field(value.document);
}

template <class Archive>
void visit_fields(Archive& archive, CounterDelta& value) {
  archive.field(value.delta);
  archive.field(value.elapsed_nanos);
  archive.field(value.spans_gap);
  archive.field(value.from_wrap);
}

template <class Archive>
void visit_fields(Archive& archive, EvidenceRef& value) {
  archive.field(value.source);
  archive.field(value.sequence);
  archive.field(value.lane);
  archive.field(value.metric);
  archive.field(value.observed_at);
  archive.field(value.received);
  archive.field(value.origin);
  archive.field(value.evidence_class);
  archive.field(value.authority);
  archive.field(value.unit);
  archive.field(value.value);
  archive.field(value.counter_reading);
  archive.field(value.delta);
  archive.field(value.used);
  archive.field(value.fresh);
  archive.field(value.age_nanos);
  archive.field(value.rendered);
  archive.field(value.provenance);
}

template <class Archive>
void visit_fields(Archive& archive, MetricAssessment& value) {
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.state);
  archive.field(value.confidence);
  archive.field(value.reasons);
  archive.field(value.evidence);
  archive.field(value.rules);
  archive.field(value.value);
  archive.field(value.rate_per_second);
  archive.field(value.disagreement);
  archive.field(value.missing_lanes);
  archive.field(value.sources_declared);
  archive.field(value.sources_with_evidence);
  archive.field(value.sources_fresh);
  archive.field(value.sources_conflicting);
  archive.field(value.capability_declared);
  archive.field(value.interval_rate_per_second);
}

template <class Archive>
void visit_fields(Archive& archive, LinkQualityReport& value) {
  archive.field(value.link);
  archive.field(value.link_registered);
  archive.field(value.evidence_present);
  archive.field(value.policy);
  archive.field(value.epoch);
  archive.field(value.overall);
  archive.field(value.confidence);
  archive.field(value.reasons);
  archive.field(value.metrics);
  archive.field(value.evidence_considered);
  archive.field(value.evidence_fresh);
  archive.field(value.evidence_recovered);
  archive.field(value.generated_wall_nanos);
  archive.field(value.generated_steady_nanos);
  archive.field(value.truncated);
}

template <class Archive>
void visit_fields(Archive& archive, ConflictEntry& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.claimants);
  archive.field(value.spread);
  archive.field(value.tolerance);
}

template <class Archive>
void visit_fields(Archive& archive, StaleEntry& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.source);
  archive.field(value.origin);
  archive.field(value.age_nanos);
  archive.field(value.received);
  archive.field(value.reason);
}

template <class Archive>
void visit_fields(Archive& archive, FreshnessEntry& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.fresh_sources);
  archive.field(value.stale_sources);
  archive.field(value.recovered_sources);
  archive.field(value.declared_sources);
  archive.field(value.newest_age_nanos);
  archive.field(value.has_fresh_evidence);
}

template <class Archive>
void visit_fields(Archive& archive, InspectionReport& value) {
  archive.field(value.policy);
  archive.field(value.epoch);
  archive.field(value.generated_wall_nanos);
  archive.field(value.conflicts);
  archive.field(value.stale);
  archive.field(value.freshness);
  archive.field(value.links_inspected);
  archive.field(value.entries_dropped);
  archive.field(value.truncated);
}

template <class Archive>
void visit_fields(Archive& archive, WindowResult& value) {
  archive.field(value.records);
  archive.field(value.matched);
  archive.field(value.returned);
  archive.field(value.truncated);
  archive.field(value.earliest_available_nanos);
  archive.field(value.latest_available_nanos);
  archive.field(value.has_available_range);
  archive.field(value.as_of_report);
}

template <class Archive>
void visit_fields(Archive& archive, QualityQuery& value) {
  archive.field(value.link);
  archive.field(value.lane);
  archive.field(value.metrics);
  archive.field(value.include_evidence);
  archive.field(value.max_metrics);
  archive.field(value.max_evidence_per_metric);
}

template <class Archive>
void visit_fields(Archive& archive, WindowQuery& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.source);
  archive.field(value.axis);
  archive.field(value.from_nanos);
  archive.field(value.to_nanos);
  archive.field(value.limit);
  archive.field(value.include_evidence);
  archive.field(value.assess_as_of);
  archive.field(value.as_of_steady_nanos);
  archive.field(value.max_metrics);
  archive.field(value.max_evidence_per_metric);
}

template <class Archive>
void visit_fields(Archive& archive, InspectionFilter& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.max_entries);
  archive.field(value.max_claimants_per_conflict);
}

template <class Archive>
void visit_fields(Archive& archive, Explanation& value) {
  archive.field(value.report);
  archive.field(value.text);
}

template <class Archive>
void visit_fields(Archive& archive, StreamKey& value) {
  archive.field(value.link);
  archive.field(value.metric);
  archive.field(value.lane);
  archive.field(value.source);
}

template <class Archive>
void visit_fields(Archive& archive, MetricDescriptor& value) {
  archive.field(value.id);
  archive.field(value.unit);
  archive.field(value.semantics);
  archive.field(value.basis);
  archive.field(value.counter_width);
  archive.field(value.supports_lanes);
  archive.field(value.description);
}

// Codec limits derived from the runtime limits, so the decoder bounds and the
// storage bounds can never disagree.
LQF_API CodecLimits codec_limits_for(const FabricLimits& limits);

}  // namespace lqf

#endif  // LQF_PERSIST_CODEC_HPP
