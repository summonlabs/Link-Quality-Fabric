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

#include "harness.hpp"

#include "lqf/store/capability_registry.hpp"
#include "lqf/store/link_registry.hpp"

using namespace lqf;
using namespace lqf::test;

LQF_TEST(identity_capability, identity_validation) {
  LQF_CHECK_STATUS_OK(validate_link_identity(LinkIdentity(LinkId(std::string("link-1")),
                                                          LinkGeneration(1))));
  LQF_CHECK(!validate_link_identity(LinkIdentity()).ok());
  LQF_CHECK(!validate_link_identity(LinkIdentity(LinkId(std::string("link 1")), LinkGeneration(1)))
                 .ok());
  LQF_CHECK(!validate_link_identity(LinkIdentity(LinkId(std::string("link-1")), LinkGeneration(0)))
                 .ok());
  LQF_CHECK(!validate_link_identity(
                 LinkIdentity(LinkId(std::string(200, 'x')), LinkGeneration(1)))
                 .ok());
  LQF_CHECK(!validate_source_identity(SourceIdentity(SourceId(std::string("s")),
                                                     SourceIncarnation(0)))
                 .ok());
  LQF_CHECK_STATUS_OK(validate_source_identity(SourceIdentity(SourceId(std::string("s")),
                                                              SourceIncarnation(1))));
  LQF_CHECK(!validate_lane_id(8, 8).ok());
  LQF_CHECK_STATUS_OK(validate_lane_id(7, 8));
  LQF_CHECK(!validate_lane_id(0, 0).ok());
  LQF_CHECK_EQ(render_link_identity(LinkIdentity(LinkId(std::string("l")), LinkGeneration(3))),
               std::string("l/gen3"));
  LQF_CHECK_EQ(render_source_identity(SourceIdentity(SourceId(std::string("s")),
                                                     SourceIncarnation(4))),
               std::string("s@4"));
}

LQF_TEST(identity_capability, capability_validation_rules) {
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  ObservationLimits limits;
  const LinkIdentity link(LinkId(std::string("link-1")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-1")), SourceIncarnation(1));

  CapabilityDeclaration good =
      make_capability(link, source, {MetricId(MetricFamily::SignalPower, "rx.level")});
  LQF_CHECK_STATUS_OK(validate_capability(good, catalog, limits, 16));

  CapabilityDeclaration unknown_metric = good;
  unknown_metric.metrics[0].metric = MetricId(MetricFamily::SignalPower, "rx.unknown");
  LQF_CHECK(validate_capability(unknown_metric, catalog, limits, 16).code() ==
            StatusCode::Unsupported);

  CapabilityDeclaration wrong_unit = good;
  wrong_unit.metrics[0].unit = Unit::NanoSecond;
  LQF_CHECK(!validate_capability(wrong_unit, catalog, limits, 16).ok());

  CapabilityDeclaration gauge_in_count = good;
  gauge_in_count.metrics[0].unit = Unit::Count;
  LQF_CHECK(!validate_capability(gauge_in_count, catalog, limits, 16).ok());

  CapabilityDeclaration lanes_without_count = good;
  lanes_without_count.metrics[0].lanes_declared = true;
  lanes_without_count.metrics[0].lane_count = 0;
  LQF_CHECK(!validate_capability(lanes_without_count, catalog, limits, 16).ok());

  CapabilityDeclaration too_many_lanes = good;
  too_many_lanes.metrics[0].lanes_declared = true;
  too_many_lanes.metrics[0].lane_count = limits.max_lanes + 1;
  LQF_CHECK(!validate_capability(too_many_lanes, catalog, limits, 16).ok());

  CapabilityDeclaration empty;
  empty.source = source;
  LQF_CHECK(!validate_capability(empty, catalog, limits, 16).ok());

  CapabilityDeclaration contradictory = good;
  contradictory.unsupported_metrics = {good.metrics[0].metric};
  LQF_CHECK(validate_capability(contradictory, catalog, limits, 16).code() == StatusCode::Conflict);

  CapabilityDeclaration duplicated = good;
  duplicated.metrics.push_back(good.metrics[0]);
  LQF_CHECK(!validate_capability(duplicated, catalog, limits, 16).ok());

  CapabilityDeclaration over_limit = good;
  LQF_CHECK(validate_capability(over_limit, catalog, limits, 0).code() == StatusCode::LimitExceeded);
}

LQF_TEST(identity_capability, capability_registry_fencing) {
  FabricLimits limits;
  CapabilityRegistry registry(limits);
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  const LinkIdentity link(LinkId(std::string("link-1")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-1")), SourceIncarnation(1));
  const MetricId metric(MetricFamily::SignalPower, "rx.level");

  CapabilityDeclaration declaration = make_capability(link, source, {metric});
  declaration.revision = CapabilityRevision(2);
  LQF_CHECK_STATUS_OK(registry.declare(declaration, catalog));
  LQF_CHECK_EQ(registry.sources_for(link, metric).size(), std::size_t{1});

  // The same revision with the same content is a duplicate.
  LQF_CHECK(registry.declare(declaration, catalog).code() == StatusCode::Duplicate);

  // The same revision with different content is an identity mismatch.
  CapabilityDeclaration changed = declaration;
  changed.metrics[0].validity_nanos = 5'000'000'000LL;
  LQF_CHECK(registry.declare(changed, catalog).code() == StatusCode::IdMismatch);

  // An older revision is fenced and cannot replace a newer one.
  CapabilityDeclaration older = declaration;
  older.revision = CapabilityRevision(1);
  LQF_CHECK(registry.declare(older, catalog).code() == StatusCode::Fenced);

  // A newer revision replaces it.
  CapabilityDeclaration newer = declaration;
  newer.revision = CapabilityRevision(3);
  newer.unsupported_metrics = {MetricId(MetricFamily::Timing, "latency.roundtrip")};
  LQF_CHECK_STATUS_OK(registry.declare(newer, catalog));
  const std::vector<MetricCapabilityView> views = registry.view_for_link(link);
  // One view for the measured metric and one for the metric the newest revision
  // declares as not measured.
  LQF_CHECK_EQ(views.size(), std::size_t{2});
  for (const MetricCapabilityView& view : views) {
    if (view.metric == metric) {
      LQF_CHECK_EQ(view.declared_by.size(), std::size_t{1});
      LQF_CHECK_EQ(view.declared_by[0].revision.value(), u64{3});
    } else {
      LQF_CHECK(view.explicitly_unsupported());
    }
  }
}

LQF_TEST(identity_capability, capability_scope_and_reincarnation) {
  FabricLimits limits;
  CapabilityRegistry registry(limits);
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  const LinkIdentity link_a(LinkId(std::string("link-a")), LinkGeneration(1));
  const LinkIdentity link_b(LinkId(std::string("link-b")), LinkGeneration(1));
  const SourceIdentity source(SourceId(std::string("src-1")), SourceIncarnation(1));
  const MetricId metric(MetricFamily::SignalRatio, "snr");

  CapabilityDeclaration global = make_capability(link_a, source, {metric});
  global.link_scope.reset();
  LQF_CHECK_STATUS_OK(registry.declare(global, catalog));
  // An unscoped declaration covers every link this source feeds.
  LQF_CHECK_EQ(registry.sources_for(link_a, metric).size(), std::size_t{1});
  LQF_CHECK_EQ(registry.sources_for(link_b, metric).size(), std::size_t{1});

  // A scoped declaration for another link does not leak into this one.
  const SourceIdentity other(SourceId(std::string("src-2")), SourceIncarnation(1));
  CapabilityDeclaration scoped = make_capability(link_b, other, {metric});
  LQF_CHECK_STATUS_OK(registry.declare(scoped, catalog));
  LQF_CHECK_EQ(registry.sources_for(link_a, metric).size(), std::size_t{1});
  LQF_CHECK_EQ(registry.sources_for(link_b, metric).size(), std::size_t{2});

  // A reincarnated source is a different declarer: its capabilities never merge
  // with the previous incarnation.
  const SourceIdentity reincarnated(SourceId(std::string("src-1")), SourceIncarnation(2));
  CapabilityDeclaration after_restart = make_capability(link_a, reincarnated, {metric});
  LQF_CHECK_STATUS_OK(registry.declare(after_restart, catalog));
  LQF_CHECK_EQ(registry.sources_for(link_a, metric).size(), std::size_t{2});
  const std::optional<MetricCapability> old_capability =
      registry.capability_of(link_a, metric, source);
  const std::optional<MetricCapability> new_capability =
      registry.capability_of(link_a, metric, reincarnated);
  LQF_CHECK(old_capability.has_value());
  LQF_CHECK(new_capability.has_value());

  // Explicitly unsupported metrics are visible as such.
  CapabilityDeclaration denial;
  denial.source = SourceIdentity(SourceId(std::string("src-3")), SourceIncarnation(1));
  denial.revision = CapabilityRevision(1);
  denial.unsupported_metrics = {MetricId(MetricFamily::Timing, "latency.roundtrip")};
  LQF_CHECK_STATUS_OK(registry.declare(denial, catalog));
  const std::vector<MetricCapabilityView> views = registry.view_for_link(link_a);
  bool found = false;
  for (const MetricCapabilityView& view : views) {
    if (view.metric == MetricId(MetricFamily::Timing, "latency.roundtrip")) {
      found = true;
      LQF_CHECK(view.explicitly_unsupported());
      LQF_CHECK(!view.supported());
    }
  }
  LQF_CHECK(found);
}

LQF_TEST(identity_capability, link_registry_generation_fencing) {
  FabricLimits limits;
  LinkRegistry registry(limits);
  const LinkId link_id(std::string("link-1"));
  const LinkIdentity first(link_id, LinkGeneration(1));
  const LinkIdentity second(link_id, LinkGeneration(2));
  const Timestamp at{1000, true};
  const ReceiveStamp received{1000, 10, FabricEpoch(1)};

  LQF_CHECK_STATUS_OK(registry.observe(first, at, received));
  LQF_CHECK(registry.is_current(first));
  LQF_CHECK(!registry.is_current(second));

  LQF_CHECK_STATUS_OK(registry.advance_generation(link_id, LinkGeneration(2), at, received));
  LQF_CHECK(registry.is_current(second));
  LQF_CHECK(!registry.is_current(first));
  const LinkState* old_state = registry.find(first);
  LQF_CHECK(old_state != nullptr);
  LQF_CHECK(old_state->superseded);
  LQF_CHECK_EQ(old_state->superseded_by.value(), u64{2});

  // A generation cannot go backwards.
  LQF_CHECK(registry.advance_generation(link_id, LinkGeneration(1), at, received).code() ==
            StatusCode::Fenced);
  // Advancing to the current generation is an idempotent no-op.
  LQF_CHECK_STATUS_OK(registry.advance_generation(link_id, LinkGeneration(2), at, received));
  LQF_CHECK_EQ(registry.current_generation(link_id).value().value(), u64{2});
}

LQF_TEST(identity_capability, link_registry_eviction_is_bounded) {
  FabricLimits limits;
  limits.max_generations_per_link = 2;
  LinkRegistry registry(limits);
  const LinkId link_id(std::string("link-1"));
  const Timestamp at{1000, true};
  const ReceiveStamp received{1000, 10, FabricEpoch(1)};
  for (u64 generation = 1; generation <= 5; ++generation) {
    LQF_CHECK_STATUS_OK(registry.advance_generation(link_id, LinkGeneration(generation), at,
                                                     received));
  }
  // Only the retained generations remain, and every eviction is reported so the
  // caller can drop the matching evidence.
  const std::vector<LinkIdentity> evicted = registry.take_evicted();
  LQF_CHECK_EQ(evicted.size(), std::size_t{3});
  LQF_CHECK_EQ(evicted[0].generation.value(), u64{1});
  LQF_CHECK_EQ(evicted[2].generation.value(), u64{3});
  LQF_CHECK(registry.find(LinkIdentity(link_id, LinkGeneration(4))) != nullptr);
  LQF_CHECK(registry.find(LinkIdentity(link_id, LinkGeneration(5))) != nullptr);
  LQF_CHECK(registry.find(LinkIdentity(link_id, LinkGeneration(1))) == nullptr);
}
