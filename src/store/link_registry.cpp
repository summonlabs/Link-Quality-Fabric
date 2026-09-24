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

#include "lqf/store/link_registry.hpp"

#include <algorithm>

#include "lqf/core/text.hpp"

namespace lqf {

Status LinkRegistry::observe(const LinkIdentity& link, const Timestamp& observed_at,
                             const ReceiveStamp& received) {
  const Status validation = validate_link_identity(link);
  if (!validation.ok()) {
    return validation;
  }
  const auto current = current_.find(link.id);
  if (current == current_.end()) {
    if (current_.size() >= limits_.max_links) {
      return Status::error(StatusCode::LimitExceeded,
                           "link identity capacity of " + std::to_string(limits_.max_links) +
                               " reached");
    }
    current_.emplace(link.id, link.generation);
    generations_[link.id].push_back(link);
  } else if (link.generation > current->second) {
    const Status advanced = advance_generation(link.id, link.generation, observed_at, received);
    if (!advanced.ok()) {
      return advanced;
    }
  }
  return note_evidence(link, observed_at, received);
}

Status LinkRegistry::note_evidence(const LinkIdentity& link, const Timestamp& observed_at,
                                   const ReceiveStamp& received) {
  auto entry = links_.find(link);
  if (entry == links_.end()) {
    LinkState state;
    state.identity = link;
    state.first_seen = observed_at;
    state.first_received = received;
    state.evidence_count = 1;
    const auto current = current_.find(link.id);
    if (current != current_.end() && link.generation < current->second) {
      state.superseded = true;
      state.superseded_by = current->second;
    }
    links_.emplace(link, std::move(state));
    return Status::success();
  }
  entry->second.evidence_count += 1;
  return Status::success();
}

Status LinkRegistry::advance_generation(const LinkId& link, LinkGeneration generation,
                                        const Timestamp& at, const ReceiveStamp& received) {
  const Status validation =
      validate_identity_text(link.value(), kMaxIdentityBytes, "link id");
  if (!validation.ok()) {
    return validation;
  }
  if (generation.is_zero()) {
    return Status::error(StatusCode::Invalid, "link generation must be at least 1");
  }
  const auto current = current_.find(link);
  if (current != current_.end()) {
    if (generation < current->second) {
      return Status::error(StatusCode::Fenced,
                           "generation " + text::format_u64(generation.value()) +
                               " is older than the current generation " +
                               text::format_u64(current->second.value()));
    }
    if (generation == current->second) {
      return Status::success();
    }
    current->second = generation;
  } else {
    if (current_.size() >= limits_.max_links) {
      return Status::error(StatusCode::LimitExceeded,
                           "link identity capacity of " + std::to_string(limits_.max_links) +
                               " reached");
    }
    current_.emplace(link, generation);
  }

  for (auto& entry : links_) {
    if (entry.first.id == link && entry.first.generation < generation) {
      entry.second.superseded = true;
      entry.second.superseded_by = generation;
    }
  }

  std::vector<LinkIdentity>& generations = generations_[link];
  const LinkIdentity identity(link, generation);
  if (std::find(generations.begin(), generations.end(), identity) == generations.end()) {
    generations.push_back(identity);
  }
  while (generations.size() > limits_.max_generations_per_link) {
    const LinkIdentity oldest = generations.front();
    generations.erase(generations.begin());
    links_.erase(oldest);
    evicted_.push_back(oldest);
  }
  LinkState state;
  state.identity = identity;
  state.first_seen = at;
  state.first_received = received;
  state.evidence_count = 0;
  links_.emplace(identity, std::move(state));
  return Status::success();
}

Status LinkRegistry::mark_registered(const LinkIdentity& link, const Timestamp& at,
                                     const ReceiveStamp& received) {
  return observe(link, at, received);
}

bool LinkRegistry::contains(const LinkIdentity& link) const {
  return links_.find(link) != links_.end();
}

const LinkState* LinkRegistry::find(const LinkIdentity& link) const {
  const auto entry = links_.find(link);
  return entry == links_.end() ? nullptr : &entry->second;
}

std::optional<LinkGeneration> LinkRegistry::current_generation(const LinkId& link) const {
  const auto entry = current_.find(link);
  if (entry == current_.end()) {
    return std::nullopt;
  }
  return entry->second;
}

bool LinkRegistry::is_current(const LinkIdentity& link) const {
  const auto entry = current_.find(link.id);
  return entry != current_.end() && entry->second == link.generation;
}

std::vector<LinkState> LinkRegistry::snapshot() const {
  std::vector<LinkState> result;
  result.reserve(links_.size());
  for (const auto& entry : links_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<LinkIdentity> LinkRegistry::take_evicted() {
  std::vector<LinkIdentity> result;
  result.swap(evicted_);
  return result;
}

void LinkRegistry::clear() noexcept {
  links_.clear();
  current_.clear();
  generations_.clear();
  evicted_.clear();
}

}  // namespace lqf
