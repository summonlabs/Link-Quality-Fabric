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

#ifndef LQF_STORE_CAPABILITY_REGISTRY_HPP
#define LQF_STORE_CAPABILITY_REGISTRY_HPP

#include <map>
#include <optional>
#include <vector>

#include "lqf/domain/capability.hpp"
#include "lqf/runtime/config.hpp"

namespace lqf {

// Declared measurement capability per source and scope. Not internally
// synchronized: the owning Fabric serialises all access, which keeps the lock
// graph acyclic and the invariants one-line provable.
class CapabilityRegistry {
 public:
  explicit CapabilityRegistry(const FabricLimits& limits) : limits_(limits) {}

  // A declaration with a revision that is not newer than the stored one is
  // fenced. Re-declaring the same revision with identical content is a
  // duplicate; with different content it is an identity mismatch.
  Status declare(const CapabilityDeclaration& declaration, const MetricCatalog& catalog);

  [[nodiscard]] std::vector<MetricCapabilityView> view_for_link(const LinkIdentity& link) const;
  [[nodiscard]] std::vector<DeclaredSource> sources_for(const LinkIdentity& link,
                                                        const MetricId& metric) const;
  [[nodiscard]] std::vector<MetricId> declared_metrics(const LinkIdentity& link) const;
  [[nodiscard]] std::optional<MetricCapability> capability_of(const LinkIdentity& link,
                                                              const MetricId& metric,
                                                              const SourceIdentity& source) const;
  [[nodiscard]] std::vector<CapabilityDeclaration> declarations() const;
  [[nodiscard]] std::size_t size() const noexcept { return declarations_.size(); }
  void clear() noexcept { declarations_.clear(); }

 private:
  struct ScopedKey {
    SourceIdentity source{};
    bool link_scoped{false};
    LinkIdentity link{};

    friend bool operator==(const ScopedKey&, const ScopedKey&) = default;
    friend auto operator<=>(const ScopedKey&, const ScopedKey&) = default;
  };

  [[nodiscard]] bool scoped_to(const CapabilityDeclaration& declaration,
                               const LinkIdentity& link) const;

  const FabricLimits limits_;
  std::map<ScopedKey, CapabilityDeclaration> declarations_{};
};

}  // namespace lqf

#endif  // LQF_STORE_CAPABILITY_REGISTRY_HPP
