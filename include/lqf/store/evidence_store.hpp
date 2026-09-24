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

#ifndef LQF_STORE_EVIDENCE_STORE_HPP
#define LQF_STORE_EVIDENCE_STORE_HPP

#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "lqf/domain/classification.hpp"
#include "lqf/domain/counter.hpp"
#include "lqf/domain/observation.hpp"
#include "lqf/runtime/config.hpp"

namespace lqf {

struct StreamStats {
  u64 accepted{0};
  u64 duplicates{0};
  u64 reordered{0};
  u64 rejected{0};
  u64 evicted{0};
  u64 missing_samples{0};
  u64 continuity_breaks{0};
};

struct StreamState {
  StreamKey key{};
  std::deque<EvidenceRecord> recent{};
  std::map<u64, u64> dedup{};  // sequence -> payload digest, bounded window
  bool has_watermark{false};
  u64 watermark{0};
  bool has_fed_sequence{false};
  u64 fed_sequence{0};
  CounterContinuity counter{};
  CounterEvent last_counter_event{CounterEvent::Baseline};
  ReasonCode last_counter_reason{ReasonCode::None};
  std::optional<CounterDelta> last_delta{};
  bool rate_admissible{false};
  // Most recent record fed to the continuity engine. Kept separately from the
  // arrival ordered ring so a snapshot can always rebuild continuity exactly.
  std::optional<EvidenceRecord> latest_fed{};
  StreamStats stats{};

  [[nodiscard]] const EvidenceRecord* newest() const noexcept {
    return recent.empty() ? nullptr : &recent.back();
  }
};

struct IngestOutcome {
  bool accepted{false};
  bool duplicate{false};
  bool reordered{false};
  ReasonCode reason{ReasonCode::None};
  CounterEvent counter_event{CounterEvent::Baseline};
  bool continuity_broken{false};
  bool rate_admissible{false};
  std::size_t stream_depth{0};
  std::size_t evicted{0};
};

// Bounded evidence storage. Not internally synchronized: the owning Fabric
// serialises access.
class EvidenceStore {
 public:
  explicit EvidenceStore(const FabricLimits& limits) : limits_(limits) {}

  // The origin is supplied by the caller: live for ingestion, recovered for
  // replay. Recovered evidence is never fresh, and that distinction must not be
  // erased by the store.
  Outcome<IngestOutcome> append(const Observation& observation, const ReceiveStamp& received,
                              IngestOrdinal ordinal, EvidenceOrigin origin,
                              const CounterContinuityConfig& counter_config);

  [[nodiscard]] const StreamState* find(const StreamKey& key) const;
  [[nodiscard]] std::vector<const StreamState*> streams_for(const LinkIdentity& link,
                                                            const MetricId& metric) const;
  [[nodiscard]] std::vector<const StreamState*> streams_for_link(const LinkIdentity& link) const;

  struct WindowScan {
    std::vector<EvidenceRecord> records{};
    u64 matched{0};
    bool truncated{false};
    i64 earliest{0};
    i64 latest{0};
    bool has_range{false};
  };

  [[nodiscard]] WindowScan window(const WindowQuery& query) const;

  // Records retained for snapshot compaction, in deterministic order.
  [[nodiscard]] std::vector<EvidenceRecord> retained_records() const;

  // Replay fence state, persisted so that duplicate and stale-id decisions after
  // a restart are identical to the decisions taken before the restart.
  [[nodiscard]] std::vector<std::pair<u64, u64>> dedup_window(const StreamKey& key) const;
  [[nodiscard]] std::vector<StreamKey> stream_keys() const;
  Status seed_dedup_window(const StreamKey& key, const std::vector<std::pair<u64, u64>>& entries);

  [[nodiscard]] std::size_t stream_count() const noexcept { return streams_.size(); }
  [[nodiscard]] std::size_t record_count() const noexcept { return history_.size(); }
  [[nodiscard]] const std::deque<EvidenceRecord>& history() const noexcept { return history_; }

  // Drops every stream of one link generation. Used when a link generation is
  // evicted by the retention bound.
  std::size_t drop_link(const LinkIdentity& link);

  void clear() noexcept;

 private:
  [[nodiscard]] bool admit_stream(const StreamKey& key);

  const FabricLimits limits_;
  std::map<StreamKey, StreamState> streams_{};
  std::deque<EvidenceRecord> history_{};
  std::map<LinkIdentity, std::size_t> streams_per_link_{};
};

LQF_API u64 evidence_digest(const Observation& observation);

}  // namespace lqf

#endif  // LQF_STORE_EVIDENCE_STORE_HPP
