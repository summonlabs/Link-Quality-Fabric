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

#include "lqf/classify/classifier.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct Candidate {
  EvidenceRef reference{};
  double value{0.0};
  AuthorityRank authority{};
  SourceIdentity source{};
};

struct LaneContext {
  bool has_evidence{false};
  bool has_fresh_reading{false};
  bool classifiable{false};
  ReasonList reasons{};
  std::vector<Candidate> candidates{};
  std::vector<EvidenceRef> evidence{};
  std::vector<Candidate> conflicting{};
  std::optional<double> interval_rate{};
  ReasonCode counter_reason{ReasonCode::None};
  CounterEvent counter_event{CounterEvent::Baseline};
  bool recovered_only{true};
  std::uint32_t sources_with_evidence{0};
  std::uint32_t sources_fresh{0};
  i64 newest_fresh_age{0};
  bool has_fresh_age{false};
};

// Evidence received after the instant being evaluated cannot be evidence for
// that instant. This is what makes an as-of query a real historical question
// rather than a relabelled current query.
bool received_after(const EvidenceRecord& record, const ClockReading& now, FabricEpoch epoch) {
  if (record.origin != EvidenceOrigin::Live || record.received.epoch != epoch) {
    return false;
  }
  return record.received.steady_nanos > now.steady_nanos;
}

const EvidenceRecord* newest_at_or_before(const StreamState& stream, const ClockReading& now,
                                          FabricEpoch epoch) {
  if (stream.latest_fed.has_value() && !received_after(*stream.latest_fed, now, epoch)) {
    return &*stream.latest_fed;
  }
  for (auto entry = stream.recent.rbegin(); entry != stream.recent.rend(); ++entry) {
    if (!received_after(*entry, now, epoch)) {
      return &*entry;
    }
  }
  return nullptr;
}

CounterObservation counter_observation_of(const EvidenceRecord& record) {
  CounterObservation observation;
  const auto* counter = as_counter(record.observation.reading);
  if (counter != nullptr) {
    observation.value = counter->value;
    observation.width = counter->width;
    observation.reset_declared = counter->reset_declared;
  }
  observation.observed_nanos = record.observation.observed_at.unix_nanos;
  observation.received_steady_nanos = record.received.steady_nanos;
  observation.sequence = record.observation.sequence;
  observation.incarnation = record.observation.source.incarnation;
  observation.link_generation = record.observation.link.generation;
  return observation;
}

i64 validity_for(const PolicyGenerationRecord& policy, const MetricRule* rule, MetricFamily family,
                 const DeclaredSource* declaration) {
  if (rule != nullptr && rule->validity_nanos > 0) {
    return rule->validity_nanos;
  }
  if (declaration != nullptr && declaration->validity_nanos > 0) {
    return declaration->validity_nanos;
  }
  return policy.document.freshness.validity_for(family);
}

void collect_lane(const ClassifierContext& context, const PolicyGenerationRecord& policy,
                  const LinkIdentity& link, const MetricId& metric, const LaneDimension& lane,
                  const std::vector<DeclaredSource>& declared, LaneContext& lane_context,
                  std::size_t max_evidence) {
  const MetricRule* rule = policy.document.find_rule(metric);
  const std::vector<const StreamState*> streams = context.store->streams_for(link, metric);
  std::set<SourceIdentity> declared_sources;
  for (const DeclaredSource& source : declared) {
    declared_sources.insert(source.source);
  }

  for (const StreamState* stream : streams) {
    if (!(stream->key.lane == lane)) {
      continue;
    }
    const DeclaredSource* declaration = nullptr;
    for (const DeclaredSource& source : declared) {
      if (source.source == stream->key.source) {
        declaration = &source;
        break;
      }
    }
    if (declaration == nullptr) {
      continue;  // evidence from a source that withdrew its declaration
    }

    const i64 validity = validity_for(policy, rule, metric.family, declaration);

    const EvidenceRecord* record = newest_at_or_before(*stream, context.now, context.epoch);
    if (record == nullptr) {
      continue;
    }
    // When the newest record is in the future of the evaluated instant the
    // continuity state belongs to a later step, so it is recomputed from the
    // retained records with the same engine and the same configuration.
    const bool reconstruct = !stream->latest_fed.has_value() ||
                             (record != &*stream->latest_fed) || received_after(*record, context.now, context.epoch);
    lane_context.has_evidence = true;
    lane_context.sources_with_evidence += 1;
    if (record->origin == EvidenceOrigin::Live) {
      lane_context.recovered_only = false;
    }

    i64 age = 0;
    const bool fresh = record_is_fresh(*record, validity, context.now, context.epoch, age);
    if (fresh) {
      lane_context.sources_fresh += 1;
      if (!lane_context.has_fresh_age || age < lane_context.newest_fresh_age) {
        lane_context.newest_fresh_age = age;
        lane_context.has_fresh_age = true;
      }
    }

    EvidenceRef reference;
    reference.source = record->observation.source;
    reference.sequence = record->observation.sequence;
    reference.lane = record->observation.lane;
    reference.metric = record->observation.metric;
    reference.observed_at = record->observation.observed_at;
    reference.received = record->received;
    reference.origin = record->origin;
    reference.evidence_class = record->observation.provenance.evidence_class;
    reference.authority = record->observation.authority;
    reference.unit = record->observation.unit;
    reference.fresh = fresh;
    reference.age_nanos = age;
    reference.rendered = render_reading(record->observation.reading, record->observation.unit);
    reference.provenance = record->observation.provenance.origin.empty()
                               ? record->observation.provenance.producer
                               : record->observation.provenance.origin;

    if (const auto* gauge = as_gauge(record->observation.reading)) {
      reference.value = gauge->value;
      lane_context.has_fresh_reading = lane_context.has_fresh_reading || fresh;
      if (fresh) {
        Candidate candidate;
        candidate.value = gauge->value;
        candidate.authority = record->observation.authority;
        candidate.source = record->observation.source;
        candidate.reference = reference;
        lane_context.candidates.push_back(std::move(candidate));
      }
    } else if (const auto* counter = as_counter(record->observation.reading)) {
      // The reading itself is fresh evidence even when no rate can be derived
      // from it: that distinction is what separates INCOMPLETE from STALE.
      lane_context.has_fresh_reading = lane_context.has_fresh_reading || fresh;
      reference.counter_reading = counter->value;
      std::optional<CounterDelta> delta = stream->last_delta;
      bool rate_admissible = stream->rate_admissible;
      CounterEvent counter_event = stream->last_counter_event;
      ReasonCode counter_reason = stream->last_counter_reason;
      if (reconstruct) {
        CounterContinuity continuity;
        CounterStep step;
        const CounterContinuityConfig config =
            counter_config_for(policy.document, stream->key.metric);
        for (const EvidenceRecord& candidate : stream->recent) {
          if (received_after(candidate, context.now, context.epoch)) {
            continue;
          }
          if (is_counter(candidate.observation.reading)) {
            step = continuity.observe(counter_observation_of(candidate), config);
          }
        }
        delta = step.delta;
        rate_admissible = continuity.rate_admissible();
        counter_event = step.event;
        counter_reason = step.reason;
      }
      lane_context.counter_event = counter_event;
      lane_context.counter_reason = counter_reason;
      if (delta.has_value()) {
        reference.delta = delta;
        const std::optional<double> rate =
            delta->rate_per_second(policy.document.freshness.min_rate_interval_nanos);
        const bool interval_inside_validity = delta->elapsed_nanos <= validity;
        if (rate.has_value() && rate_admissible && fresh && interval_inside_validity) {
          reference.value = *rate;
          lane_context.has_fresh_reading = true;
          Candidate candidate;
          candidate.value = *rate;
          candidate.authority = record->observation.authority;
          candidate.source = record->observation.source;
          candidate.reference = reference;
          lane_context.candidates.push_back(std::move(candidate));
        } else if (rate.has_value()) {
          lane_context.interval_rate = *rate;
          add_reason(lane_context.reasons, rate_admissible ? ReasonCode::PartiallyFreshEvidence
                                                          : ReasonCode::ContinuityGapExceeded);
        }
      } else {
        add_reason(lane_context.reasons, counter_reason);
      }
    }
    if (lane_context.evidence.size() < max_evidence) {
      lane_context.evidence.push_back(std::move(reference));
    }
  }
}

ReasonCode counter_incomplete_reason(const LaneContext& lane_context) {
  if (lane_context.counter_reason != ReasonCode::None) {
    return lane_context.counter_reason;
  }
  return ReasonCode::CounterBaselineOnly;
}

ConfidenceLevel confidence_for(QualityState state, std::size_t distinct_sources, i64 age,
                               bool has_age, i64 validity) {
  switch (state) {
    case QualityState::Unknown:
    case QualityState::Unsupported:
      return ConfidenceLevel::None;
    case QualityState::Stale:
    case QualityState::Incomplete:
    case QualityState::Conflicting:
      return ConfidenceLevel::Low;
    default:
      break;
  }
  if (distinct_sources >= 2) {
    return ConfidenceLevel::High;
  }
  if (!has_age || validity <= 0) {
    return ConfidenceLevel::Medium;
  }
  return age * 2 <= validity ? ConfidenceLevel::Medium : ConfidenceLevel::Low;
}

MetricAssessment assess_lane(const ClassifierContext& context, const PolicyGenerationRecord& policy,
                             const LinkIdentity& link, const MetricId& metric,
                             const LaneDimension& lane, const MetricCapabilityView& view,
                             const QualityQuery& query) {
  MetricAssessment assessment;
  assessment.metric = metric;
  assessment.lane = lane;
  assessment.capability_declared = view.supported();
  assessment.sources_declared = static_cast<std::uint32_t>(view.declared_by.size());

  const MetricRule* rule = policy.document.find_rule(metric);
  const i64 validity =
      validity_for(policy, rule, metric.family,
                   view.declared_by.empty() ? nullptr : &view.declared_by.front());

  if (!view.supported()) {
    assessment.state = QualityState::Unsupported;
    assessment.confidence = ConfidenceLevel::None;
    add_reason(assessment.reasons, view.explicitly_unsupported() ? ReasonCode::DeclaredUnsupported
                                                                 : ReasonCode::MetricNotDeclared);
    return assessment;
  }

  LaneContext lane_context;
  collect_lane(context, policy, link, metric, lane, view.declared_by, lane_context,
               query.max_evidence_per_metric);

  assessment.sources_with_evidence = lane_context.sources_with_evidence;
  assessment.sources_fresh = lane_context.sources_fresh;
  assessment.evidence = lane_context.evidence;
  assessment.interval_rate_per_second = lane_context.interval_rate;
  for (const ReasonCode reason : lane_context.reasons) {
    add_reason(assessment.reasons, reason);
  }

  if (!lane_context.has_evidence) {
    assessment.state = QualityState::Unknown;
    assessment.confidence = ConfidenceLevel::None;
    add_reason(assessment.reasons, ReasonCode::NoEvidence);
    return assessment;
  }

  if (lane_context.candidates.empty()) {
    if (!lane_context.has_fresh_reading) {
      assessment.state = QualityState::Stale;
      assessment.confidence = ConfidenceLevel::Low;
      add_reason(assessment.reasons, lane_context.recovered_only
                                         ? ReasonCode::RecoveredEvidenceOnly
                                         : ReasonCode::EvidenceExpired);
      return assessment;
    }
    assessment.state = QualityState::Incomplete;
    assessment.confidence = ConfidenceLevel::Low;
    add_reason(assessment.reasons, counter_incomplete_reason(lane_context));
    return assessment;
  }

  // Strictly greater authority wins outright; equal authority must agree.
  AuthorityRank best{};
  for (const Candidate& candidate : lane_context.candidates) {
    if (candidate.authority > best) {
      best = candidate.authority;
    }
  }
  std::vector<Candidate> winners;
  for (const Candidate& candidate : lane_context.candidates) {
    if (candidate.authority == best) {
      winners.push_back(candidate);
    } else {
      add_reason(assessment.reasons, ReasonCode::LowerAuthorityIgnored);
    }
  }
  std::sort(winners.begin(), winners.end(), [](const Candidate& lhs, const Candidate& rhs) {
    if (!(lhs.source == rhs.source)) {
      return lhs.source < rhs.source;
    }
    return lhs.reference.sequence < rhs.reference.sequence;
  });

  const double tolerance = rule != nullptr ? rule->conflict_tolerance : 0.0;
  double lowest = winners.front().value;
  double highest = winners.front().value;
  for (const Candidate& candidate : winners) {
    lowest = std::min(lowest, candidate.value);
    highest = std::max(highest, candidate.value);
  }
  double spread = highest - lowest;
  if (!std::isfinite(spread)) {
    spread = std::numeric_limits<double>::infinity();
  }
  assessment.disagreement = spread;

  std::set<SourceIdentity> distinct;
  for (const Candidate& candidate : winners) {
    distinct.insert(candidate.source);
  }

  if (winners.size() >= 2 && !(spread <= tolerance)) {
    assessment.state = QualityState::Conflicting;
    assessment.confidence = ConfidenceLevel::Low;
    assessment.sources_conflicting = static_cast<std::uint32_t>(winners.size());
    add_reason(assessment.reasons, ReasonCode::EqualAuthorityDisagreement);
    return assessment;
  }

  if (rule == nullptr) {
    assessment.state = QualityState::Unknown;
    assessment.confidence = ConfidenceLevel::None;
    add_reason(assessment.reasons, ReasonCode::NoRuleForMetric);
    return assessment;
  }

  const double value = winners.front().value;
  assessment.value = value;
  if (metric.family == MetricFamily::ErrorCounter || rule->basis != MetricBasis::GaugeValue) {
    assessment.rate_per_second = value;
  }
  const ThresholdBand* band = rule->classify(value);
  if (band == nullptr) {
    assessment.state = QualityState::Unknown;
    assessment.confidence = ConfidenceLevel::None;
    return assessment;
  }
  assessment.state = band->state;
  assessment.rules.push_back(band->id);
  add_reason(assessment.reasons, ReasonCode::BandMatched);
  assessment.confidence = confidence_for(assessment.state, distinct.size(),
                                         lane_context.newest_fresh_age,
                                         lane_context.has_fresh_age, validity);
  return assessment;
}

MetricAssessment aggregate_lanes(const ClassifierContext& context,
                                 const PolicyGenerationRecord& policy, const LinkIdentity& link,
                                 const MetricId& metric, const MetricCapabilityView& view,
                                 const std::vector<LaneId>& lanes, const QualityQuery& query) {
  MetricAssessment aggregate;
  aggregate.metric = metric;
  aggregate.lane = LaneDimension::aggregate_dimension();
  aggregate.capability_declared = view.supported();

  std::vector<MetricAssessment> per_lane;
  for (const LaneId& lane : lanes) {
    per_lane.push_back(assess_lane(context, policy, link, metric, LaneDimension(lane), view, query));
  }
  if (per_lane.empty()) {
    return assess_lane(context, policy, link, metric, LaneDimension::aggregate_dimension(), view,
                       query);
  }

  const SeverityTable& severity = policy.document.aggregation.severity;
  QualityState worst = per_lane.front().state;
  const MetricAssessment* worst_assessment = &per_lane.front();
  for (const MetricAssessment& assessment : per_lane) {
    if (severity.of(assessment.state) > severity.of(worst)) {
      worst = assessment.state;
      worst_assessment = &assessment;
    }
  }
  aggregate.state = worst;
  aggregate.confidence = worst_assessment->confidence;
  aggregate.value = worst_assessment->value;
  aggregate.rate_per_second = worst_assessment->rate_per_second;
  aggregate.interval_rate_per_second = worst_assessment->interval_rate_per_second;
  aggregate.rules = worst_assessment->rules;
  for (const MetricAssessment& assessment : per_lane) {
    for (const ReasonCode reason : assessment.reasons) {
      add_reason(aggregate.reasons, reason);
    }
    aggregate.sources_declared = std::max(aggregate.sources_declared, assessment.sources_declared);
    aggregate.sources_with_evidence += assessment.sources_with_evidence;
    aggregate.sources_fresh += assessment.sources_fresh;
    aggregate.sources_conflicting += assessment.sources_conflicting;
    if (assessment.confidence < aggregate.confidence) {
      aggregate.confidence = assessment.confidence;
    }
    if (assessment.state == QualityState::Conflicting) {
      add_reason(aggregate.reasons, ReasonCode::EqualAuthorityDisagreement);
    }
    if (assessment.state == QualityState::Unsupported ||
        assessment.state == QualityState::Unknown) {
      aggregate.missing_lanes.push_back(assessment.lane.value());
      add_reason(aggregate.reasons, ReasonCode::MissingLanes);
    }
    for (const EvidenceRef& reference : assessment.evidence) {
      if (aggregate.evidence.size() < query.max_evidence_per_metric) {
        aggregate.evidence.push_back(reference);
      }
    }
  }
  // An aggregate that hides an unmeasured lane would be a false roll-up.
  if (!aggregate.missing_lanes.empty()) {
    const QualityState with_missing = severity.worst(aggregate.state, QualityState::Incomplete);
    if (with_missing != aggregate.state) {
      add_reason(aggregate.reasons, ReasonCode::LaneCoveragePartial);
    }
    aggregate.state = with_missing;
  }
  return aggregate;
}

std::vector<MetricId> resolve_metrics(const QualityQuery& query,
                                      const std::vector<MetricCapabilityView>& views) {
  std::vector<MetricId> metrics;
  if (!query.metrics.empty()) {
    metrics = query.metrics;
    if (metrics.size() > query.max_metrics) {
      metrics.resize(query.max_metrics);
    }
    std::sort(metrics.begin(), metrics.end());
    metrics.erase(std::unique(metrics.begin(), metrics.end()), metrics.end());
  } else {
    for (const MetricCapabilityView& view : views) {
      metrics.push_back(view.metric);
      if (metrics.size() >= query.max_metrics) {
        break;
      }
    }
  }
  std::sort(metrics.begin(), metrics.end());
  return metrics;
}

const MetricCapabilityView* find_view(const std::vector<MetricCapabilityView>& views,
                                      const MetricId& metric) {
  for (const MetricCapabilityView& view : views) {
    if (view.metric == metric) {
      return &view;
    }
  }
  return nullptr;
}

}  // namespace

Status ClassifierContext::validate() const {
  if (links == nullptr || store == nullptr || capabilities == nullptr || policy == nullptr ||
      limits == nullptr) {
    return Status::error(StatusCode::Internal, "classifier context is incomplete");
  }
  return Status::success();
}

bool record_is_fresh(const EvidenceRecord& record, i64 validity_nanos, const ClockReading& now,
                     FabricEpoch epoch, i64& age_nanos) {
  if (record.origin != EvidenceOrigin::Live || record.received.epoch != epoch) {
    // Recovered evidence can never be fresh, whatever the clock says. The age is
    // reported from the wall clock so an operator can see how old it really is.
    const i64 wall_age = now.wall_nanos - record.received.wall_nanos;
    age_nanos = wall_age < 0 ? 0 : wall_age;
    return false;
  }
  const i64 steady_age = now.steady_nanos - record.received.steady_nanos;
  age_nanos = steady_age < 0 ? 0 : steady_age;
  if (validity_nanos <= 0) {
    return false;
  }
  return age_nanos <= validity_nanos;
}

LinkQualityReport classify_link(const ClassifierContext& context, const QualityQuery& query) {
  LinkQualityReport report;
  report.link = query.link;
  report.policy = context.policy->stamp;
  report.epoch = context.epoch;
  report.generated_wall_nanos = context.now.wall_nanos;
  report.generated_steady_nanos = context.now.steady_nanos;

  const Status validation = context.validate();
  if (!validation.ok()) {
    report.overall = QualityState::Unknown;
    add_reason(report.reasons, ReasonCode::NoSourcesForLink);
    return report;
  }

  const LinkState* link_state = context.links->find(query.link);
  report.link_registered = link_state != nullptr;
  if (link_state == nullptr) {
    add_reason(report.reasons, ReasonCode::LinkNotRegistered);
  } else if (link_state->superseded) {
    add_reason(report.reasons, ReasonCode::FencedGeneration);
  }

  const std::vector<MetricCapabilityView> views = context.capabilities->view_for_link(query.link);
  const std::vector<MetricId> metrics = resolve_metrics(query, views);

  if (views.empty()) {
    report.overall = QualityState::Unsupported;
    report.confidence = ConfidenceLevel::None;
    add_reason(report.reasons, ReasonCode::NoSourcesForLink);
    return report;
  }
  if (metrics.empty()) {
    report.overall = QualityState::Unsupported;
    report.confidence = ConfidenceLevel::None;
    add_reason(report.reasons, ReasonCode::MetricNotDeclared);
    return report;
  }

  for (const MetricId& metric : metrics) {
    const MetricCapabilityView* view = find_view(views, metric);
    MetricCapabilityView standalone;
    if (view == nullptr) {
      standalone.metric = metric;
      view = &standalone;
      if (std::find(context.policy->document.unsupported_metrics.begin(),
                    context.policy->document.unsupported_metrics.end(),
                    metric) != context.policy->document.unsupported_metrics.end()) {
        standalone.declared_unsupported_by.push_back(DeclaredSource{});
      }
    }

    if (query.lane.has_value() && !query.lane->is_aggregate()) {
      report.metrics.push_back(
          assess_lane(context, *context.policy, query.link, metric, *query.lane, *view, query));
      continue;
    }

    std::vector<LaneId> lanes;
    u32 declared_lanes = 0;
    for (const DeclaredSource& source : view->declared_by) {
      if (source.lanes_declared) {
        declared_lanes = std::max(declared_lanes, source.lane_count);
      }
    }
    for (u32 lane = 0; lane < declared_lanes; ++lane) {
      lanes.push_back(LaneId(lane));
    }
    if (lanes.empty()) {
      report.metrics.push_back(assess_lane(context, *context.policy, query.link, metric,
                                           LaneDimension::aggregate_dimension(), *view, query));
    } else {
      report.metrics.push_back(
          aggregate_lanes(context, *context.policy, query.link, metric, *view, lanes, query));
    }
  }

  const SeverityTable& severity = context.policy->document.aggregation.severity;
  QualityState overall = QualityState::Unknown;
  bool first = true;
  ConfidenceLevel confidence = ConfidenceLevel::High;
  bool any_conflict = false;
  for (const MetricAssessment& assessment : report.metrics) {
    if (first) {
      overall = assessment.state;
      confidence = assessment.confidence;
      first = false;
    } else {
      overall = severity.worst(overall, assessment.state);
      if (assessment.confidence < confidence) {
        confidence = assessment.confidence;
      }
    }
    if (assessment.state == QualityState::Conflicting) {
      any_conflict = true;
    }
    for (const ReasonCode reason : assessment.reasons) {
      add_reason(report.reasons, reason);
    }
    report.evidence_considered += assessment.evidence.size();
    for (const EvidenceRef& reference : assessment.evidence) {
      if (reference.fresh) {
        report.evidence_fresh += 1;
      }
      if (reference.origin == EvidenceOrigin::Recovered) {
        report.evidence_recovered += 1;
      }
      if (reference.origin == EvidenceOrigin::Live && !reference.fresh) {
        report.evidence_present = true;
      }
      if (reference.origin == EvidenceOrigin::Live) {
        report.evidence_present = true;
      }
    }
  }
  if (any_conflict && context.policy->document.aggregation.conflicting_dominates) {
    overall = QualityState::Conflicting;
    confidence = ConfidenceLevel::Low;
    add_reason(report.reasons, ReasonCode::EqualAuthorityDisagreement);
  }
  report.overall = overall;
  report.confidence = first ? ConfidenceLevel::None : confidence;
  if (first) {
    add_reason(report.reasons, ReasonCode::NoEvidence);
  }
  return report;
}

InspectionReport inspect_fabric(const ClassifierContext& context, const InspectionFilter& filter) {
  InspectionReport report;
  report.policy = context.policy->stamp;
  report.epoch = context.epoch;
  report.generated_wall_nanos = context.now.wall_nanos;

  if (!context.validate().ok()) {
    return report;
  }

  std::vector<LinkIdentity> links;
  for (const LinkState& state : context.links->snapshot()) {
    if (filter.link.has_value() && !(state.identity == *filter.link)) {
      continue;
    }
    links.push_back(state.identity);
  }
  std::sort(links.begin(), links.end());
  report.links_inspected = links.size();
  if (links.size() > filter.max_entries) {
    report.entries_dropped += links.size() - filter.max_entries;
    links.resize(filter.max_entries);
    report.truncated = true;
  }

  for (const LinkIdentity& link : links) {
    QualityQuery query;
    query.link = link;
    query.include_evidence = true;
    query.max_metrics = context.limits->max_metrics_per_query;
    query.max_evidence_per_metric = context.limits->max_evidence_per_metric;
    LinkQualityReport quality = classify_link(context, query);

    for (const MetricAssessment& assessment : quality.metrics) {
      if (filter.metric.has_value() && !(assessment.metric == *filter.metric)) {
        continue;
      }
      FreshnessEntry freshness;
      freshness.link = link;
      freshness.metric = assessment.metric;
      freshness.lane = assessment.lane;
      freshness.declared_sources = assessment.sources_declared;
      freshness.fresh_sources = assessment.sources_fresh;
      freshness.has_fresh_evidence = assessment.sources_fresh > 0;
      freshness.newest_age_nanos = 0;
      for (const EvidenceRef& reference : assessment.evidence) {
        if (reference.fresh) {
          freshness.newest_age_nanos =
              freshness.has_fresh_evidence
                  ? std::min(freshness.newest_age_nanos, reference.age_nanos)
                  : reference.age_nanos;
        }
        if (reference.origin == EvidenceOrigin::Recovered) {
          freshness.recovered_sources += 1;
          continue;
        }
        if (!reference.fresh) {
          freshness.stale_sources += 1;
          if (report.stale.size() < filter.max_entries) {
            StaleEntry entry;
            entry.link = link;
            entry.metric = assessment.metric;
            entry.lane = assessment.lane;
            entry.source = reference.source;
            entry.origin = reference.origin;
            entry.age_nanos = reference.age_nanos;
            entry.received = reference.received;
            entry.reason = reference.origin == EvidenceOrigin::Recovered
                               ? ReasonCode::RecoveredEvidenceOnly
                               : ReasonCode::EvidenceExpired;
            report.stale.push_back(std::move(entry));
          } else {
            report.entries_dropped += 1;
            report.truncated = true;
          }
        }
      }
      if (assessment.state == QualityState::Conflicting) {
        if (report.conflicts.size() < filter.max_entries) {
          ConflictEntry entry;
          entry.link = link;
          entry.metric = assessment.metric;
          entry.lane = assessment.lane;
          entry.spread = assessment.disagreement.value_or(0.0);
          const MetricRule* rule = context.policy->document.find_rule(assessment.metric);
          entry.tolerance = rule != nullptr ? rule->conflict_tolerance : 0.0;
          for (const EvidenceRef& reference : assessment.evidence) {
            if (entry.claimants.size() >= filter.max_claimants_per_conflict) {
              break;
            }
            entry.claimants.push_back(reference);
          }
          report.conflicts.push_back(std::move(entry));
        } else {
          report.entries_dropped += 1;
          report.truncated = true;
        }
      }
      if (report.freshness.size() < filter.max_entries) {
        report.freshness.push_back(std::move(freshness));
      } else {
        report.entries_dropped += 1;
        report.truncated = true;
      }
    }
  }
  return report;
}

}  // namespace lqf
