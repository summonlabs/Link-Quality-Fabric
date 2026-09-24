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

#include "lqf/store/capability_registry.hpp"

#include <algorithm>
#include <set>

#include "lqf/core/text.hpp"
#include "lqf/persist/archive.hpp"
#include "lqf/persist/codec.hpp"

namespace lqf {
namespace {

std::string canonical_declaration(const CapabilityDeclaration& declaration) {
  CodecLimits limits;
  limits.max_string_bytes = 256;
  limits.max_items = 4096;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(declaration);
  return writer.ok() ? writer.buffer() : std::string();
}

}  // namespace

Status CapabilityRegistry::declare(const CapabilityDeclaration& declaration,
                                   const MetricCatalog& catalog) {
  ObservationLimits observation_limits;
  observation_limits.max_origin_bytes = limits_.max_string_bytes;
  observation_limits.max_lanes = 4096;
  const Status validation = validate_capability(declaration, catalog, observation_limits,
                                                limits_.max_metrics_per_capability);
  if (!validation.ok()) {
    return validation;
  }
  ScopedKey key;
  key.source = declaration.source;
  key.link_scoped = declaration.link_scope.has_value();
  key.link = declaration.link_scope.value_or(LinkIdentity{});

  auto existing = declarations_.find(key);
  if (existing == declarations_.end()) {
    if (declarations_.size() >= limits_.max_capability_declarations) {
      return Status::error(StatusCode::LimitExceeded,
                           "capability declaration capacity reached");
    }
    declarations_.emplace(key, declaration);
    return Status::success();
  }
  if (declaration.revision < existing->second.revision) {
    return Status::error(StatusCode::Fenced,
                         "capability revision " + text::format_u64(declaration.revision.value()) +
                             " is older than the stored revision " +
                             text::format_u64(existing->second.revision.value()));
  }
  if (declaration.revision == existing->second.revision) {
    if (canonical_declaration(declaration) == canonical_declaration(existing->second)) {
      return Status::error(StatusCode::Duplicate, "capability revision already recorded");
    }
    return Status::error(StatusCode::IdMismatch,
                         "capability revision re-used with different content");
  }
  existing->second = declaration;
  return Status::success();
}

bool CapabilityRegistry::scoped_to(const CapabilityDeclaration& declaration,
                                   const LinkIdentity& link) const {
  if (!declaration.link_scope.has_value()) {
    return true;
  }
  return *declaration.link_scope == link;
}

std::vector<MetricCapabilityView> CapabilityRegistry::view_for_link(const LinkIdentity& link) const {
  std::map<MetricId, MetricCapabilityView> views;
  for (const auto& entry : declarations_) {
    const CapabilityDeclaration& declaration = entry.second;
    if (!scoped_to(declaration, link)) {
      continue;
    }
    for (const MetricCapability& capability : declaration.metrics) {
      MetricCapabilityView& view = views[capability.metric];
      view.metric = capability.metric;
      DeclaredSource source;
      source.source = declaration.source;
      source.revision = declaration.revision;
      source.evidence_class = declaration.evidence_class;
      source.transport = declaration.transport;
      source.lanes_declared = capability.lanes_declared;
      source.lane_count = capability.lane_count;
      source.unit = capability.unit;
      source.semantics = capability.semantics;
      source.counter_width = capability.counter_width;
      source.validity_nanos = capability.validity_nanos;
      view.declared_by.push_back(std::move(source));
    }
    for (const MetricId& metric : declaration.unsupported_metrics) {
      MetricCapabilityView& view = views[metric];
      view.metric = metric;
      DeclaredSource source;
      source.source = declaration.source;
      source.revision = declaration.revision;
      source.evidence_class = declaration.evidence_class;
      source.transport = declaration.transport;
      view.declared_unsupported_by.push_back(std::move(source));
    }
  }
  std::vector<MetricCapabilityView> result;
  result.reserve(views.size());
  for (auto& entry : views) {
    std::sort(entry.second.declared_by.begin(), entry.second.declared_by.end(),
              [](const DeclaredSource& lhs, const DeclaredSource& rhs) {
                return lhs.source < rhs.source;
              });
    std::sort(entry.second.declared_unsupported_by.begin(),
              entry.second.declared_unsupported_by.end(),
              [](const DeclaredSource& lhs, const DeclaredSource& rhs) {
                return lhs.source < rhs.source;
              });
    result.push_back(std::move(entry.second));
  }
  return result;
}

std::vector<DeclaredSource> CapabilityRegistry::sources_for(const LinkIdentity& link,
                                                            const MetricId& metric) const {
  std::vector<DeclaredSource> sources;
  for (const MetricCapabilityView& view : view_for_link(link)) {
    if (view.metric == metric) {
      sources = view.declared_by;
      break;
    }
  }
  return sources;
}

std::vector<MetricId> CapabilityRegistry::declared_metrics(const LinkIdentity& link) const {
  std::vector<MetricId> metrics;
  for (const MetricCapabilityView& view : view_for_link(link)) {
    metrics.push_back(view.metric);
  }
  return metrics;
}

std::optional<MetricCapability> CapabilityRegistry::capability_of(const LinkIdentity& link,
                                                                 const MetricId& metric,
                                                                 const SourceIdentity& source) const {
  for (const auto& entry : declarations_) {
    const CapabilityDeclaration& declaration = entry.second;
    if (!(declaration.source == source) || !scoped_to(declaration, link)) {
      continue;
    }
    for (const MetricCapability& capability : declaration.metrics) {
      if (capability.metric == metric) {
        return capability;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::vector<CapabilityDeclaration> CapabilityRegistry::declarations() const {
  std::vector<CapabilityDeclaration> result;
  result.reserve(declarations_.size());
  for (const auto& entry : declarations_) {
    result.push_back(entry.second);
  }
  return result;
}

}  // namespace lqf
