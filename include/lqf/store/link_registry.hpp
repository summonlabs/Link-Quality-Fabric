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

#ifndef LQF_STORE_LINK_REGISTRY_HPP
#define LQF_STORE_LINK_REGISTRY_HPP

#include <map>
#include <optional>
#include <set>
#include <vector>

#include "lqf/domain/identity.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/runtime/config.hpp"

namespace lqf {

// Bookkeeping of which link identities and generations this runtime has seen.
// This is not link authority: registering a generation records that evidence
// arrived for it, and nothing here decides whether a link is up, usable or
// eligible for anything.
struct LinkState {
  LinkIdentity identity{};
  Timestamp first_seen{};
  ReceiveStamp first_received{};
  u64 evidence_count{0};
  bool superseded{false};
  LinkGeneration superseded_by{};
};

class LinkRegistry {
 public:
  explicit LinkRegistry(const FabricLimits& limits) : limits_(limits) {}

  Status observe(const LinkIdentity& link, const Timestamp& observed_at,
                 const ReceiveStamp& received);
  Status advance_generation(const LinkId& link, LinkGeneration generation, const Timestamp& at,
                            const ReceiveStamp& received);
  Status mark_registered(const LinkIdentity& link, const Timestamp& at, const ReceiveStamp& received);

  [[nodiscard]] bool contains(const LinkIdentity& link) const;
  [[nodiscard]] const LinkState* find(const LinkIdentity& link) const;
  [[nodiscard]] std::optional<LinkGeneration> current_generation(const LinkId& link) const;
  [[nodiscard]] bool is_current(const LinkIdentity& link) const;
  [[nodiscard]] std::vector<LinkState> snapshot() const;
  [[nodiscard]] std::size_t size() const noexcept { return links_.size(); }
  // Generations dropped because the per-link retention bound was reached. The
  // caller drops the matching evidence so memory stays bounded.
  [[nodiscard]] std::vector<LinkIdentity> take_evicted();
  void clear() noexcept;

 private:
  Status note_evidence(const LinkIdentity& link, const Timestamp& observed_at,
                       const ReceiveStamp& received);

  const FabricLimits limits_;
  std::map<LinkIdentity, LinkState> links_{};
  std::map<LinkId, LinkGeneration> current_{};
  std::map<LinkId, std::vector<LinkIdentity>> generations_{};
  std::vector<LinkIdentity> evicted_{};
};

}  // namespace lqf

#endif  // LQF_STORE_LINK_REGISTRY_HPP
