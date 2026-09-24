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

#ifndef LQF_RUNTIME_FABRIC_HPP
#define LQF_RUNTIME_FABRIC_HPP

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lqf/classify/classifier.hpp"
#include "lqf/core/clock.hpp"
#include "lqf/core/lock_tracker.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/persist/journal.hpp"
#include "lqf/runtime/config.hpp"
#include "lqf/store/capability_registry.hpp"
#include "lqf/store/evidence_store.hpp"
#include "lqf/store/link_registry.hpp"
#include "lqf/transport/messages.hpp"

namespace lqf {

struct FabricStats {
  FabricEpoch epoch{};
  PolicyStamp policy{};
  u64 observations_accepted{0};
  u64 observations_duplicate{0};
  u64 observations_reordered{0};
  u64 observations_rejected{0};
  u64 observations_recovered{0};
  u64 ingest_batches{0};
  u64 capability_declarations{0};
  u64 capability_rejections{0};
  u64 policy_publications{0};
  u64 policy_rejections{0};
  u64 links_observed{0};
  u64 links_registered{0};
  u64 generations_advanced{0};
  u64 queries_served{0};
  u64 window_queries{0};
  u64 explanations{0};
  u64 inspections{0};
  u64 stream_evictions{0};
  u64 history_evictions{0};
  u64 continuity_breaks{0};
  u64 wraps_inferred{0};
  u64 resets_observed{0};
  u64 unproven_decreases{0};
  u64 limit_rejections{0};
  u64 journal_records{0};
  u64 journal_bytes{0};
  u64 journal_rejections{0};
  u64 compactions{0};
  u64 recovered_records{0};
  u64 recoveries{0};
  std::size_t links{0};
  std::size_t streams{0};
  std::size_t retained_records{0};
  bool journal_enabled{false};
  bool open{true};
  RecoverySummary recovery{};
};

struct BatchOutcome {
  u64 accepted{0};
  u64 duplicates{0};
  u64 reordered{0};
  u64 rejected{0};
  StatusCode first_code{StatusCode::Ok};
  ReasonCode first_reason{ReasonCode::None};
};

// The runtime façade. Every public entry point is thread safe; internally all
// mutable state sits behind one audited reader/writer lock, and no callback,
// join or foreign lock is ever taken while it is held.
class LQF_API Fabric {
 public:
  static Outcome<std::shared_ptr<Fabric>> open(FabricConfig config);
  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  // --- capability registration -------------------------------------------
  Outcome<CapabilityAck> declare_capability(const CapabilityDeclaration& declaration);
  [[nodiscard]] Outcome<std::vector<MetricCapabilityView>> capabilities(
      const LinkIdentity& link) const;
  [[nodiscard]] std::vector<MetricDescriptor> metric_catalog() const;

  // --- evidence ingestion -------------------------------------------------
  Outcome<IngestOutcome> ingest(const Observation& observation);
  Outcome<BatchOutcome> ingest_batch(const std::vector<Observation>& observations);

  // --- link generations ---------------------------------------------------
  Status register_link(const LinkIdentity& link, const Timestamp& observed_at);
  Status advance_link_generation(const LinkId& link, LinkGeneration generation,
                                 ReasonCode reason);

  // --- queries ------------------------------------------------------------
  [[nodiscard]] Outcome<LinkQualityReport> query(const QualityQuery& request) const;
  [[nodiscard]] Outcome<WindowResult> window(const WindowQuery& request) const;
  [[nodiscard]] Outcome<Explanation> explain(const QualityQuery& request) const;
  [[nodiscard]] Outcome<InspectionReport> inspect(const InspectionFilter& filter) const;

  // --- policy generations -------------------------------------------------
  Outcome<PolicyStamp> publish_policy(const PolicyDocument& document);
  [[nodiscard]] PolicyStamp current_policy() const;
  [[nodiscard]] std::vector<PolicyStamp> policy_history() const;
  [[nodiscard]] Outcome<PolicyGenerationRecord> policy_document(PolicyGeneration generation) const;

  // --- persistence --------------------------------------------------------
  Status flush();
  Status compact();
  Status close();
  [[nodiscard]] const RecoveryReport& recovery_report() const noexcept { return recovery_; }
  [[nodiscard]] JournalStats journal_stats() const;

  // --- introspection ------------------------------------------------------
  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] FabricEpoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] ClockReading now() const { return clock_->now(); }
  [[nodiscard]] const FabricLimits& limits() const noexcept { return config_.limits; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

 private:
  explicit Fabric(FabricConfig config);

  Status initialize();
  Status apply_record(const JournalRecord& record, bool recovering, int depth);
  // Allocates the next ordinal and persists one record. Only called while the
  // state lock is held, which is what makes the ordinal order equal to the file
  // order and the queue order.
  Status journal_append(JournalRecordKind kind, std::string body);
  [[nodiscard]] Status build_snapshot(std::string& out, IngestOrdinal& ordinal) const;
  void maybe_compact();

  const FabricConfig config_;
  const std::shared_ptr<Clock> clock_;
  const FabricEpoch epoch_;
  mutable locks::TrackedMutex mutex_{"fabric_state"};

  MetricCatalog catalog_{};
  PolicyRegistry policies_;
  CapabilityRegistry capabilities_;
  LinkRegistry links_;
  EvidenceStore evidence_;
  std::unique_ptr<BackgroundJournal> journal_{};

  IngestOrdinal next_ordinal_{};
  u64 records_since_compaction_{0};
  bool persistence_degraded_{false};
  // Read path counters are atomic: readers hold a shared lock and must never
  // write shared state.
  mutable std::atomic<u64> queries_served_{0};
  mutable std::atomic<u64> window_queries_{0};
  mutable std::atomic<u64> explanations_{0};
  mutable std::atomic<u64> inspections_{0};
  FabricStats stats_{};
  RecoveryReport recovery_{};
  bool open_{true};
};

template <class Archive>
void visit_fields(Archive& archive, FabricStats& value) {
  archive.field(value.epoch);
  archive.field(value.policy);
  archive.field(value.observations_accepted);
  archive.field(value.observations_duplicate);
  archive.field(value.observations_reordered);
  archive.field(value.observations_rejected);
  archive.field(value.observations_recovered);
  archive.field(value.ingest_batches);
  archive.field(value.capability_declarations);
  archive.field(value.capability_rejections);
  archive.field(value.policy_publications);
  archive.field(value.policy_rejections);
  archive.field(value.links_observed);
  archive.field(value.links_registered);
  archive.field(value.generations_advanced);
  archive.field(value.queries_served);
  archive.field(value.window_queries);
  archive.field(value.explanations);
  archive.field(value.inspections);
  archive.field(value.stream_evictions);
  archive.field(value.history_evictions);
  archive.field(value.continuity_breaks);
  archive.field(value.wraps_inferred);
  archive.field(value.resets_observed);
  archive.field(value.unproven_decreases);
  archive.field(value.limit_rejections);
  archive.field(value.journal_records);
  archive.field(value.journal_bytes);
  archive.field(value.journal_rejections);
  archive.field(value.compactions);
  archive.field(value.recovered_records);
  archive.field(value.recoveries);
  archive.field(value.links);
  archive.field(value.streams);
  archive.field(value.retained_records);
  archive.field(value.journal_enabled);
  archive.field(value.open);
  archive.field(value.recovery);
}

}  // namespace lqf

#endif  // LQF_RUNTIME_FABRIC_HPP
