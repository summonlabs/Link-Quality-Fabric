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

#include "lqf/runtime/fabric.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "lqf/classify/explain.hpp"
#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"
#include "lqf/persist/archive.hpp"
#include "lqf/persist/codec.hpp"
#include "lqf/version.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace lqf {
namespace {

u64 process_identity() {
#if defined(_WIN32)
  return static_cast<u64>(_getpid());
#else
  return static_cast<u64>(getpid());
#endif
}

FabricEpoch derive_epoch(const ClockReading& now, u64 process_id) {
  std::random_device device;
  const u64 random_bits = (static_cast<u64>(device()) << 32U) ^ static_cast<u64>(device());
  u64 value = static_cast<u64>(now.wall_nanos) ^ (process_id << 40U) ^ random_bits;
  if (value == 0) {
    value = 1;
  }
  return FabricEpoch(value);
}

template <class T>
Status decode_body(const std::string& body, T& value, const CodecLimits& limits) {
  Reader reader(body, limits);
  Decoder decoder(reader, limits);
  decoder.field(value);
  if (!decoder.ok()) {
    return reader.status();
  }
  if (!reader.empty()) {
    return Status::error(StatusCode::Protocol, "record has trailing bytes");
  }
  return Status::success();
}

template <class T>
Status encode_body(const T& value, std::string& body, const CodecLimits& limits) {
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(value);
  if (!encoder.ok()) {
    return writer.status();
  }
  body = writer.release();
  return Status::success();
}

RecoverySummary summarize(const RecoveryReport& report) {
  RecoverySummary summary;
  summary.opened = report.opened;
  summary.header_valid = report.header_valid;
  summary.torn_tail = report.torn_tail;
  summary.degraded = report.degraded;
  summary.truncated = report.truncated_file;
  summary.snapshot_loaded = report.snapshot_loaded;
  summary.limit_reached = report.limit_reached;
  summary.records_read = report.records_read;
  summary.bytes_read = report.bytes_read;
  summary.bytes_discarded = report.bytes_discarded;
  summary.status = report.status.code();
  return summary;
}

}  // namespace

namespace detail {

// Journal record bodies that have no other home. They live in a named namespace
// so the field archive can find their visit_fields overloads.
struct LinkObservedBody {
  LinkIdentity link{};
  Timestamp observed_at{};
  ReceiveStamp received{};
};

struct GenerationBody {
  LinkId link{};
  LinkGeneration generation{};
  Timestamp at{};
  ReceiveStamp received{};
  ReasonCode reason{ReasonCode::None};
};

struct DedupBody {
  StreamKey key{};
  std::vector<std::pair<u64, u64>> entries{};
};

template <class Archive>
void visit_fields(Archive& archive, LinkObservedBody& value) {
  archive.field(value.link);
  archive.field(value.observed_at);
  archive.field(value.received);
}

template <class Archive>
void visit_fields(Archive& archive, GenerationBody& value) {
  archive.field(value.link);
  archive.field(value.generation);
  archive.field(value.at);
  archive.field(value.received);
  archive.field(value.reason);
}

template <class Archive>
void visit_fields(Archive& archive, DedupBody& value) {
  archive.field(value.key);
  archive.field(value.entries);
}

}  // namespace detail

Fabric::Fabric(FabricConfig config)
    : config_(std::move(config)),
      clock_(config_.clock != nullptr ? config_.clock : make_system_clock()),
      epoch_(config_.forced_epoch.is_zero()
                 ? derive_epoch(clock_->now(), process_identity())
                 : config_.forced_epoch),
      policies_(config_.has_initial_policy ? config_.initial_policy : default_policy_document(),
                Timestamp{clock_->now().wall_nanos, true}, config_.max_policy_generations),
      capabilities_(config_.limits),
      links_(config_.limits),
      evidence_(config_.limits) {}

Fabric::~Fabric() {
  const Status status = close();
  (void)status;
}

Outcome<std::shared_ptr<Fabric>> Fabric::open(FabricConfig config) {
  auto fabric = std::shared_ptr<Fabric>(new Fabric(std::move(config)));
  const Status status = fabric->initialize();
  if (!status.ok()) {
    return status;
  }
  return fabric;
}

Status Fabric::initialize() {
  Status status = config_.limits.validate();
  if (!status.ok()) {
    return status;
  }
  status = config_.journal.validate();
  if (!status.ok()) {
    return status;
  }
  catalog_ = MetricCatalog::with_builtin_descriptors(256);

  const PolicyGenerationRecord initial = policies_.current();
  status = validate_policy(initial.document, catalog_, config_.limits.max_policy_rules,
                           config_.limits.max_bands_per_rule);
  if (!status.ok()) {
    return Status::error(status.code(), "initial policy is invalid: " + status.message());
  }

  if (!config_.journal.enabled) {
    stats_.journal_enabled = false;
    recovery_.status = Status::success();
    recovery_.detail = "persistence is disabled";
    open_ = true;
    return Status::success();
  }
  stats_.journal_enabled = true;

  bool fresh_journal = false;
  {
    JournalReader reader;
    Outcome<RecoveryReport> opened = reader.open(config_.journal.path, config_.journal);
    if (!opened.ok()) {
      recovery_ = reader.report();
      recovery_.status = opened.status();
      return Status::error(opened.status().code(),
                           "journal could not be recovered: " + opened.status().message());
    }
    JournalRecord record;
    for (;;) {
      Outcome<bool> more = reader.next(record);
      if (!more.ok()) {
        recovery_ = reader.report();
        recovery_.status = more.status();
        return Status::error(more.status().code(),
                             "journal recovery failed: " + more.status().message());
      }
      if (!more.value()) {
        break;
      }
      const Status applied = apply_record(record, true, 0);
      if (!applied.ok()) {
        // A record that cannot be applied degrades recovery; it is never
        // silently dropped and never fabricated into the state.
        recovery_.degraded = true;
      }
    }
    recovery_ = reader.report();
    // Whether the journal was empty is only known after the replay: writing the
    // initial generation again on every open would append a duplicate record and
    // restart the ordinal sequence in the middle of the file.
    fresh_journal = recovery_.records_read == 0;
    next_ordinal_ = IngestOrdinal(reader.next_ordinal());
    if (recovery_.degraded) {
      // Recovery stopped at data that could not be verified. Serving a state
      // derived from a partial history would be a guess, so the runtime refuses
      // to open and reports exactly where it stopped.
      recovery_.status = Status::error(StatusCode::Corrupt, recovery_.detail);
      return Status::error(StatusCode::Corrupt,
                           "journal recovery stopped at unverifiable data: " + recovery_.detail);
    }
    if (recovery_.torn_tail) {
      const Status repaired = reader.repair_torn_tail();
      if (!repaired.ok()) {
        recovery_.status = repaired;
        return Status::error(repaired.code(),
                             "a damaged journal tail could not be repaired: " + repaired.message());
      }
      recovery_ = reader.report();
    }
    recovery_.status = Status::success();
  }

  journal_ = std::make_unique<BackgroundJournal>(config_.journal);
  status = journal_->open(epoch_, clock_->now());
  if (!status.ok()) {
    return status;
  }
  stats_.recoveries += 1;

  if (fresh_journal) {
    // The initial generation is persisted so a later process restores the exact
    // generation numbers it was using.
    std::string body;
    status = encode_body(initial, body, codec_limits_for(config_.limits));
    if (!status.ok()) {
      return status;
    }
    status = journal_append(JournalRecordKind::Policy, std::move(body));
    if (!status.ok()) {
      return status;
    }
  }

  if (config_.journal.compact_on_open) {
    status = compact();
    if (!status.ok() && status.code() != StatusCode::Refused) {
      return status;
    }
  }
  open_ = true;
  return Status::success();
}

Status Fabric::journal_append(JournalRecordKind kind, std::string body) {
  if (journal_ == nullptr || !journal_->enabled()) {
    return Status::success();
  }
  if (!journal_->has_capacity()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; the record was refused before the state "
                         "changed");
  }
  const Status status = journal_->append(kind, next_ordinal_, std::move(body));
  if (!status.ok()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return status;
  }
  next_ordinal_ = next_ordinal_.next();
  stats_.journal_records += 1;
  records_since_compaction_ += 1;
  return Status::success();
}

Status Fabric::apply_record(const JournalRecord& record, bool /*recovering*/, int depth) {
  const CodecLimits limits = codec_limits_for(config_.limits);
  switch (record.kind) {
    case JournalRecordKind::Capability: {
      CapabilityDeclaration declaration;
      const Status decoded = decode_body(record.body, declaration, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      return capabilities_.declare(declaration, catalog_);
    }
    case JournalRecordKind::Policy: {
      PolicyGenerationRecord policy;
      const Status decoded = decode_body(record.body, policy, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      const Status restored = policies_.restore(policy, catalog_, config_.limits.max_policy_rules,
                                                config_.limits.max_bands_per_rule);
      if (restored.code() == StatusCode::Duplicate) {
        return Status::success();
      }
      return restored;
    }
    case JournalRecordKind::LinkObserved: {
      detail::LinkObservedBody body;
      const Status decoded = decode_body(record.body, body, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      return links_.observe(body.link, body.observed_at, body.received);
    }
    case JournalRecordKind::LinkGenerationAdvanced: {
      detail::GenerationBody body;
      const Status decoded = decode_body(record.body, body, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      const Status advanced =
          links_.advance_generation(body.link, body.generation, body.at, body.received);
      if (!advanced.ok()) {
        return advanced;
      }
      for (const LinkIdentity& evicted : links_.take_evicted()) {
        evidence_.drop_link(evicted);
      }
      return Status::success();
    }
    case JournalRecordKind::Evidence: {
      EvidenceRecord stored;
      const Status decoded = decode_body(record.body, stored, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      stored.origin = EvidenceOrigin::Recovered;
      const MetricDescriptor* descriptor = catalog_.find(stored.observation.metric);
      ObservationLimits observation_limits;
      observation_limits.max_origin_bytes = config_.limits.max_string_bytes;
      observation_limits.max_producer_bytes = config_.limits.max_string_bytes;
      observation_limits.max_lanes = 4096;
      const Status valid = validate_observation(stored.observation, descriptor, observation_limits);
      if (!valid.ok()) {
        return valid;
      }
      // Capability is not re-checked: the record was accepted while it was being
      // journaled, and replay restores state instead of re-deciding it.
      const PolicyGenerationRecord policy = policies_.current();
      const CounterContinuityConfig counter_config =
          counter_config_for(policy.document, stored.observation.metric);
      Outcome<IngestOutcome> appended = evidence_.append(stored.observation, stored.received,
                                                         stored.ordinal, EvidenceOrigin::Recovered,
                                                         counter_config);
      if (!appended.ok()) {
        return appended.status();
      }
      const Status observed =
          links_.observe(stored.observation.link, stored.observation.observed_at, stored.received);
      if (!observed.ok()) {
        return observed;
      }
      for (const LinkIdentity& evicted : links_.take_evicted()) {
        evidence_.drop_link(evicted);
      }
      stats_.observations_recovered += 1;
      stats_.recovered_records += 1;
      return Status::success();
    }
    case JournalRecordKind::DedupWindow: {
      detail::DedupBody body;
      const Status decoded = decode_body(record.body, body, limits);
      if (!decoded.ok()) {
        return decoded;
      }
      return evidence_.seed_dedup_window(body.key, body.entries);
    }
    case JournalRecordKind::Snapshot: {
      if (depth > 0) {
        return Status::error(StatusCode::Corrupt, "nested snapshots are not permitted");
      }
      std::vector<JournalRecord> nested;
      const Status decoded = decode_record_bundle(record.body, nested,
                                                  config_.journal.max_recovery_records,
                                                  config_.journal.max_record_bytes);
      if (!decoded.ok()) {
        return decoded;
      }
      for (const JournalRecord& inner : nested) {
        const Status applied = apply_record(inner, true, depth + 1);
        if (!applied.ok()) {
          return applied;
        }
      }
      return Status::success();
    }
    case JournalRecordKind::Count:
    default:
      return Status::error(StatusCode::Protocol, "unknown journal record kind");
  }
}

Outcome<CapabilityAck> Fabric::declare_capability(const CapabilityDeclaration& declaration) {
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the fabric is closed");
  }
  ObservationLimits observation_limits;
  observation_limits.max_origin_bytes = config_.limits.max_string_bytes;
  observation_limits.max_producer_bytes = config_.limits.max_string_bytes;
  observation_limits.max_lanes = 4096;
  const Status valid =
      validate_capability(declaration, catalog_, observation_limits,
                          config_.limits.max_metrics_per_capability);
  if (!valid.ok()) {
    return valid;
  }

  locks::UniqueLock guard(mutex_);
  if (journal_ != nullptr && journal_->enabled() && !journal_->has_capacity()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; the declaration was refused before any "
                         "state changed");
  }
  const Status declared = capabilities_.declare(declaration, catalog_);
  if (declared.code() == StatusCode::Duplicate) {
    CapabilityAck ack;
    ack.revision = declaration.revision;
    ack.duplicate = 1;
    stats_.capability_declarations += 1;
    return ack;
  }
  if (!declared.ok()) {
    stats_.capability_rejections += 1;
    return declared;
  }
  std::string body;
  Status status = encode_body(declaration, body, codec_limits_for(config_.limits));
  if (!status.ok()) {
    return status;
  }
  const IngestOrdinal ordinal = next_ordinal_;
  const Status journaled = journal_append(JournalRecordKind::Capability, std::move(body));
  if (!journaled.ok()) {
    // The declaration is in memory but not durable: report it rather than
    // pretending the state is persisted.
    stats_.capability_declarations += 1;
    CapabilityAck ack;
    ack.revision = declaration.revision;
    (void)ordinal;
    return ack;
  }
  stats_.capability_declarations += 1;
  CapabilityAck ack;
  ack.revision = declaration.revision;
  return ack;
}

Outcome<std::vector<MetricCapabilityView>> Fabric::capabilities(const LinkIdentity& link) const {
  const Status valid = validate_link_identity(link);
  if (!valid.ok()) {
    return valid;
  }
  locks::SharedLock guard(mutex_);
  return capabilities_.view_for_link(link);
}

std::vector<MetricDescriptor> Fabric::metric_catalog() const {
  locks::SharedLock guard(mutex_);
  return catalog_.snapshot();
}

Status Fabric::register_link(const LinkIdentity& link, const Timestamp& observed_at) {
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the fabric is closed");
  }
  const Status valid = validate_link_identity(link);
  if (!valid.ok()) {
    return valid;
  }
  const ClockReading now = clock_->now();
  const ReceiveStamp received{now.wall_nanos, now.steady_nanos, epoch_};
  locks::UniqueLock guard(mutex_);
  if (links_.contains(link)) {
    return Status::success();
  }  if (journal_ != nullptr && journal_->enabled() && !journal_->has_capacity()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; the operation was refused before any state "
                         "changed");
  }
  detail::LinkObservedBody body{link, observed_at, received};
  std::string encoded;
  Status status = encode_body(body, encoded, codec_limits_for(config_.limits));
  if (!status.ok()) {
    return status;
  }
  status = links_.observe(link, observed_at, received);
  if (!status.ok()) {
    return status;
  }
  for (const LinkIdentity& evicted : links_.take_evicted()) {
    evidence_.drop_link(evicted);
  }
  stats_.links_registered += 1;
  return journal_append(JournalRecordKind::LinkObserved, std::move(encoded));
}

Status Fabric::advance_link_generation(const LinkId& link, LinkGeneration generation,
                                      ReasonCode reason) {
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the fabric is closed");
  }
  const ClockReading now = clock_->now();
  const Timestamp at{now.wall_nanos, true};
  const ReceiveStamp received{now.wall_nanos, now.steady_nanos, epoch_};
  locks::UniqueLock guard(mutex_);  if (journal_ != nullptr && journal_->enabled() && !journal_->has_capacity()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; the operation was refused before any state "
                         "changed");
  }  detail::GenerationBody body{link, generation, at, received, reason};
  std::string encoded;
  Status status = encode_body(body, encoded, codec_limits_for(config_.limits));
  if (!status.ok()) {
    return status;
  }
  status = links_.advance_generation(link, generation, at, received);
  if (!status.ok()) {
    return status;
  }
  for (const LinkIdentity& evicted : links_.take_evicted()) {
    evidence_.drop_link(evicted);
  }
  stats_.generations_advanced += 1;
  return journal_append(JournalRecordKind::LinkGenerationAdvanced, std::move(encoded));
}

Outcome<IngestOutcome> Fabric::ingest(const Observation& observation) {
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the fabric is closed");
  }
  const ClockReading now = clock_->now();
  const ReceiveStamp received{now.wall_nanos, now.steady_nanos, epoch_};
  const CodecLimits codec_limits = codec_limits_for(config_.limits);

  ObservationLimits observation_limits;
  observation_limits.max_origin_bytes = config_.limits.max_string_bytes;
  observation_limits.max_producer_bytes = config_.limits.max_string_bytes;
  observation_limits.max_lanes = 4096;

  locks::UniqueLock guard(mutex_);
  const MetricDescriptor* descriptor = catalog_.find(observation.metric);
  if (descriptor == nullptr) {
    stats_.observations_rejected += 1;
    return Status::error(StatusCode::Unsupported,
                         "metric is not registered in the catalog: " +
                             render_metric_id(observation.metric));
  }
  const Status valid = validate_observation(observation, descriptor, observation_limits);
  if (!valid.ok()) {
    stats_.observations_rejected += 1;
    return valid;
  }

  const PolicyGenerationRecord policy = policies_.current();
  if (policy.document.require_capability) {
    const std::optional<MetricCapability> capability =
        capabilities_.capability_of(observation.link, observation.metric, observation.source);
    if (!capability.has_value()) {
      stats_.observations_rejected += 1;
      return Status::error(StatusCode::Rejected,
                           "no capability declaration covers this metric for this source: " +
                               render_metric_id(observation.metric));
    }
    if (capability->unit != observation.unit) {
      stats_.observations_rejected += 1;
      return Status::error(StatusCode::Invalid,
                           "units do not match the declared capability: expected " +
                               std::string(to_string(capability->unit)) + ", received " +
                               std::string(to_string(observation.unit)));
    }
    if (capability->semantics != observation.semantics()) {
      stats_.observations_rejected += 1;
      return Status::error(StatusCode::Invalid,
                           "sample semantics do not match the declared capability");
    }
    if (!observation.lane.is_aggregate()) {
      if (!capability->lanes_declared) {
        stats_.observations_rejected += 1;
        return Status::error(StatusCode::Rejected,
                             "the declared capability does not cover per lane evidence");
      }
      if (observation.lane.value().value() >= capability->lane_count) {
        stats_.observations_rejected += 1;
        return Status::error(StatusCode::Invalid,
                             "lane exceeds the declared lane count of " +
                                 text::format_u64(capability->lane_count));
      }
    }
    if (const auto* counter = as_counter(observation.reading)) {
      if (counter->width == CounterWidth::Unspecified &&
          capability->counter_width == CounterWidth::Unspecified) {
        stats_.observations_rejected += 1;
        return Status::error(StatusCode::Invalid,
                             "counter evidence must declare a width, here or in the capability");
      }
      if (counter->width != CounterWidth::Unspecified &&
          capability->counter_width != CounterWidth::Unspecified &&
          counter->width != capability->counter_width) {
        stats_.observations_rejected += 1;
        return Status::error(StatusCode::Invalid,
                             "counter width does not match the declared capability");
      }
    }
  }

  if (observation.observed_at.synchronized && config_.limits.max_observation_age_nanos > 0) {
    const i64 age = now.wall_nanos - observation.observed_at.unix_nanos;
    if (age > config_.limits.max_observation_age_nanos) {
      stats_.observations_rejected += 1;
      return Status::error(StatusCode::Stale,
                           "evidence from a synchronized source is older than the configured "
                           "maximum observation age");
    }
  }

  const std::optional<LinkGeneration> current = links_.current_generation(observation.link.id);
  const bool fenced =
      current.has_value() && observation.link.generation < *current;

  if (journal_ != nullptr && journal_->enabled() && !journal_->has_capacity()) {
    stats_.observations_rejected += 1;
    stats_.journal_rejections += 1;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; evidence was refused before the state "
                         "changed");
  }

  const bool new_link = !links_.contains(observation.link);
  const Status observed = links_.observe(observation.link, observation.observed_at, received);
  if (!observed.ok()) {
    stats_.observations_rejected += 1;
    return observed;
  }
  for (const LinkIdentity& evicted : links_.take_evicted()) {
    evidence_.drop_link(evicted);
    stats_.history_evictions += 1;
  }
  stats_.links_observed += 1;
  if (new_link) {
    detail::LinkObservedBody body{observation.link, observation.observed_at, received};
    std::string encoded;
    const Status encoded_status = encode_body(body, encoded, codec_limits);
    if (!encoded_status.ok()) {
      stats_.observations_rejected += 1;
      return encoded_status;
    }
    const Status link_journal = journal_append(JournalRecordKind::LinkObserved, std::move(encoded));
    if (!link_journal.ok() && link_journal.code() != StatusCode::Refused) {
      stats_.observations_rejected += 1;
      return link_journal;
    }
  }

  const IngestOrdinal ordinal = next_ordinal_;
  const CounterContinuityConfig counter_config =
      counter_config_for(policy.document, observation.metric);
  Outcome<IngestOutcome> appended = evidence_.append(observation, received, ordinal,
                                                      EvidenceOrigin::Live, counter_config);
  if (!appended.ok()) {
    stats_.observations_rejected += 1;
    return appended.status();
  }

  EvidenceRecord stored;
  stored.observation = observation;
  stored.received = received;
  stored.ordinal = ordinal;
  stored.origin = EvidenceOrigin::Live;
  std::string encoded;
  const Status encoded_status = encode_body(stored, encoded, codec_limits);
  if (!encoded_status.ok()) {
    stats_.observations_rejected += 1;
    return encoded_status;
  }
  const Status journaled = journal_append(JournalRecordKind::Evidence, std::move(encoded));
  if (journal_ == nullptr || !journal_->enabled()) {
    // Without a journal the ordinal still advances so that window ordering is
    // deterministic; nothing is persisted and nothing claims to be.
    next_ordinal_ = next_ordinal_.next();
  }
  if (!journaled.ok() && journaled.code() != StatusCode::Refused) {
    // The evidence is in memory but not durable. It stays queryable and is
    // reported as such; nothing is silently dropped.
    stats_.observations_accepted += 1;
    IngestOutcome outcome = appended.value();
    outcome.reason = ReasonCode::PersistenceDegraded;
    return outcome;
  }

  IngestOutcome outcome = appended.value();
  if (outcome.duplicate) {
    stats_.observations_duplicate += 1;
  } else if (outcome.accepted) {
    stats_.observations_accepted += 1;
    if (outcome.reordered) {
      stats_.observations_reordered += 1;
    }
    if (outcome.continuity_broken) {
      stats_.continuity_breaks += 1;
    }
    switch (outcome.counter_event) {
      case CounterEvent::Wrap: stats_.wraps_inferred += 1; break;
      case CounterEvent::Reset: stats_.resets_observed += 1; break;
      case CounterEvent::DecreaseUnproven: stats_.unproven_decreases += 1; break;
      default: break;
    }
    stats_.stream_evictions += outcome.evicted;
  }
  if (fenced) {
    outcome.reason = ReasonCode::FencedGeneration;
  }
  maybe_compact();
  return outcome;
}

Outcome<BatchOutcome> Fabric::ingest_batch(const std::vector<Observation>& observations) {
  if (observations.size() > config_.limits.max_evidence_batch) {
    return Status::error(StatusCode::LimitExceeded,
                         "batch exceeds the configured evidence batch limit of " +
                             std::to_string(config_.limits.max_evidence_batch));
  }
  BatchOutcome batch;
  for (const Observation& observation : observations) {
    Outcome<IngestOutcome> outcome = ingest(observation);
    if (!outcome.ok()) {
      batch.rejected += 1;
      if (batch.first_code == StatusCode::Ok) {
        batch.first_code = outcome.status().code();
      }
      continue;
    }
    if (outcome.value().duplicate) {
      batch.duplicates += 1;
    } else if (outcome.value().reordered) {
      batch.reordered += 1;
      batch.accepted += 1;
    } else {
      batch.accepted += 1;
    }
  }
  stats_.ingest_batches += 1;
  return batch;
}


Outcome<LinkQualityReport> Fabric::query(const QualityQuery& request) const {
  const Status valid = validate_link_identity(request.link);
  if (!valid.ok()) {
    return valid;
  }
  const ClockReading now = clock_->now();
  locks::SharedLock guard(mutex_);
  PolicyGenerationRecord policy = policies_.current();
  ClassifierContext context;
  context.links = &links_;
  context.store = &evidence_;
  context.capabilities = &capabilities_;
  context.policy = &policy;
  context.limits = &config_.limits;
  context.now = now;
  context.epoch = epoch_;
  queries_served_.fetch_add(1, std::memory_order_relaxed);
  return classify_link(context, request);
}

Outcome<WindowResult> Fabric::window(const WindowQuery& request) const {
  if (request.limit == 0 || request.limit > config_.limits.max_window_records) {
    return Status::error(StatusCode::LimitExceeded,
                         "window limit must be within [1, " +
                             std::to_string(config_.limits.max_window_records) + "]");
  }
  if (request.from_nanos >= request.to_nanos) {
    return Status::error(StatusCode::Invalid, "window range is empty or inverted");
  }
  if (request.link.has_value()) {
    const Status valid = validate_link_identity(*request.link);
    if (!valid.ok()) {
      return valid;
    }
  }
  const ClockReading now = clock_->now();
  if (request.assess_as_of && request.as_of_steady_nanos > now.steady_nanos) {
    return Status::error(StatusCode::Invalid, "as-of instant is in the future");
  }
  locks::SharedLock guard(mutex_);
  WindowResult result;
  EvidenceStore::WindowScan scan = evidence_.window(request);
  result.matched = scan.matched;
  result.returned = scan.records.size();
  result.truncated = scan.truncated;
  result.earliest_available_nanos = scan.earliest;
  result.latest_available_nanos = scan.latest;
  result.has_available_range = scan.has_range;
  if (request.include_evidence) {
    result.records = std::move(scan.records);
  }
  if (request.assess_as_of && request.link.has_value()) {
    PolicyGenerationRecord policy = policies_.current();
    ClassifierContext context;
    context.links = &links_;
    context.store = &evidence_;
    context.capabilities = &capabilities_;
    context.policy = &policy;
    context.limits = &config_.limits;
    context.now = now;
    context.epoch = epoch_;
    if (request.as_of_steady_nanos > 0) {
      context.now.steady_nanos = request.as_of_steady_nanos;
      context.now.wall_nanos = now.wall_nanos - (now.steady_nanos - request.as_of_steady_nanos);
    }
    QualityQuery query;
    query.link = *request.link;
    if (request.metric.has_value()) {
      query.metrics.push_back(*request.metric);
    }
    query.lane = request.lane;
    query.max_metrics = request.max_metrics;
    query.max_evidence_per_metric = request.max_evidence_per_metric;
    result.as_of_report = classify_link(context, query);
  }
  window_queries_.fetch_add(1, std::memory_order_relaxed);
  return result;
}

Outcome<Explanation> Fabric::explain(const QualityQuery& request) const {
  const Status valid = validate_link_identity(request.link);
  if (!valid.ok()) {
    return valid;
  }
  const ClockReading now = clock_->now();
  locks::SharedLock guard(mutex_);
  PolicyGenerationRecord policy = policies_.current();
  ClassifierContext context;
  context.links = &links_;
  context.store = &evidence_;
  context.capabilities = &capabilities_;
  context.policy = &policy;
  context.limits = &config_.limits;
  context.now = now;
  context.epoch = epoch_;
  explanations_.fetch_add(1, std::memory_order_relaxed);
  return explain_link(context, request);
}

Outcome<InspectionReport> Fabric::inspect(const InspectionFilter& filter) const {
  const ClockReading now = clock_->now();
  locks::SharedLock guard(mutex_);
  PolicyGenerationRecord policy = policies_.current();
  ClassifierContext context;
  context.links = &links_;
  context.store = &evidence_;
  context.capabilities = &capabilities_;
  context.policy = &policy;
  context.limits = &config_.limits;
  context.now = now;
  context.epoch = epoch_;
  inspections_.fetch_add(1, std::memory_order_relaxed);
  return inspect_fabric(context, filter);
}

Outcome<PolicyStamp> Fabric::publish_policy(const PolicyDocument& document) {
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the fabric is closed");
  }
  const ClockReading now = clock_->now();
  locks::UniqueLock guard(mutex_);  if (journal_ != nullptr && journal_->enabled() && !journal_->has_capacity()) {
    stats_.journal_rejections += 1;
    persistence_degraded_ = true;
    return Status::error(StatusCode::Busy,
                         "the journal queue is full; the operation was refused before any state "
                         "changed");
  }  Outcome<PolicyGenerationRecord> published =
      policies_.publish(document, Timestamp{now.wall_nanos, true}, catalog_,
                        config_.limits.max_policy_rules, config_.limits.max_bands_per_rule);
  if (!published.ok()) {
    stats_.policy_rejections += 1;
    return published.status();
  }
  std::string body;
  const Status encoded = encode_body(published.value(), body, codec_limits_for(config_.limits));
  if (!encoded.ok()) {
    return encoded;
  }
  const Status journaled = journal_append(JournalRecordKind::Policy, std::move(body));
  if (!journaled.ok() && journaled.code() != StatusCode::Refused) {
    stats_.policy_publications += 1;
    return published.value().stamp;
  }
  stats_.policy_publications += 1;
  return published.value().stamp;
}

PolicyStamp Fabric::current_policy() const {
  locks::SharedLock guard(mutex_);
  return policies_.current().stamp;
}

std::vector<PolicyStamp> Fabric::policy_history() const {
  locks::SharedLock guard(mutex_);
  return policies_.history();
}

Outcome<PolicyGenerationRecord> Fabric::policy_document(PolicyGeneration generation) const {
  locks::SharedLock guard(mutex_);
  const std::optional<PolicyGenerationRecord> record = policies_.get(generation);
  if (!record.has_value()) {
    return Status::error(StatusCode::NotFound, "policy generation is not retained");
  }
  return *record;
}

Status Fabric::flush() {
  if (journal_ == nullptr || !journal_->enabled()) {
    return Status::error(StatusCode::Refused, "persistence is disabled");
  }
  return journal_->flush();
}

Status Fabric::build_snapshot(std::string& out, IngestOrdinal& ordinal) const {
  const CodecLimits limits = codec_limits_for(config_.limits);
  std::vector<JournalRecord> records;
  u64 sequence = 1;
  const auto push = [&records, &sequence](JournalRecordKind kind, std::string body) {
    JournalRecord record;
    record.kind = kind;
    record.ordinal = IngestOrdinal(sequence++);
    record.body = std::move(body);
    records.push_back(std::move(record));
  };

  for (const PolicyStamp& stamp : policies_.history()) {
    const std::optional<PolicyGenerationRecord> record = policies_.get(stamp.generation);
    if (!record.has_value()) {
      continue;
    }
    std::string body;
    const Status encoded = encode_body(*record, body, limits);
    if (!encoded.ok()) {
      return encoded;
    }
    push(JournalRecordKind::Policy, std::move(body));
  }
  for (const CapabilityDeclaration& declaration : capabilities_.declarations()) {
    std::string body;
    const Status encoded = encode_body(declaration, body, limits);
    if (!encoded.ok()) {
      return encoded;
    }
    push(JournalRecordKind::Capability, std::move(body));
  }
  for (const LinkState& state : links_.snapshot()) {
    detail::LinkObservedBody body{state.identity, state.first_seen, state.first_received};
    std::string encoded;
    const Status status = encode_body(body, encoded, limits);
    if (!status.ok()) {
      return status;
    }
    push(JournalRecordKind::LinkObserved, std::move(encoded));
  }
  for (const EvidenceRecord& record : evidence_.retained_records()) {
    std::string encoded;
    const Status status = encode_body(record, encoded, limits);
    if (!status.ok()) {
      return status;
    }
    push(JournalRecordKind::Evidence, std::move(encoded));
  }
  // The replay fence is written after the evidence it fences: seeding it first
  // would make every recovered record look like a duplicate of itself.
  for (const StreamKey& key : evidence_.stream_keys()) {
    detail::DedupBody body;
    body.key = key;
    body.entries = evidence_.dedup_window(key);
    std::string encoded;
    const Status status = encode_body(body, encoded, limits);
    if (!status.ok()) {
      return status;
    }
    push(JournalRecordKind::DedupWindow, std::move(encoded));
  }
  ordinal = next_ordinal_.is_zero() ? IngestOrdinal(0) : next_ordinal_.prev();
  return encode_record_bundle(records, out, config_.journal.max_record_bytes);
}

Status Fabric::compact() {
  if (journal_ == nullptr || !journal_->enabled()) {
    return Status::error(StatusCode::Refused, "persistence is disabled");
  }
  locks::UniqueLock guard(mutex_);
  std::string body;
  IngestOrdinal ordinal;
  const Status built = build_snapshot(body, ordinal);
  if (!built.ok()) {
    return built;
  }
  const Status status = journal_->compact(ordinal, std::move(body));
  if (status.ok()) {
    stats_.compactions += 1;
    records_since_compaction_ = 0;
  }
  return status;
}

void Fabric::maybe_compact() {
  if (journal_ == nullptr || !journal_->enabled()) {
    return;
  }
  if (config_.journal.compact_after_records == 0 ||
      records_since_compaction_ < config_.journal.compact_after_records) {
    return;
  }
  std::string body;
  IngestOrdinal ordinal;
  const Status built = build_snapshot(body, ordinal);
  if (!built.ok()) {
    return;
  }
  const Status status = journal_->compact(ordinal, std::move(body));
  if (status.ok()) {
    stats_.compactions += 1;
    records_since_compaction_ = 0;
  }
}

Status Fabric::close() {
  std::unique_ptr<BackgroundJournal> journal;
  {
    locks::UniqueLock guard(mutex_);
    if (!open_ && journal_ == nullptr) {
      return Status::success();
    }
    open_ = false;
    journal = std::move(journal_);
  }
  if (journal == nullptr) {
    return Status::success();
  }
  return journal->stop();
}

FabricStats Fabric::stats() const {
  locks::SharedLock guard(mutex_);
  FabricStats copy = stats_;
  copy.queries_served = queries_served_.load(std::memory_order_relaxed);
  copy.window_queries = window_queries_.load(std::memory_order_relaxed);
  copy.explanations = explanations_.load(std::memory_order_relaxed);
  copy.inspections = inspections_.load(std::memory_order_relaxed);
  copy.epoch = epoch_;
  copy.policy = policies_.current().stamp;
  copy.links = links_.size();
  copy.streams = evidence_.stream_count();
  copy.retained_records = evidence_.record_count();
  copy.open = open_;
  copy.recovery = summarize(recovery_);
  if (journal_ != nullptr) {
    const JournalStats journal = journal_->stats();
    copy.journal_enabled = journal.open;
    copy.journal_records = journal.records_written;
    copy.journal_bytes = journal.bytes_written;
    copy.journal_rejections += journal.rejected_appends;
    copy.compactions = journal.compactions;
  }
  return copy;
}

JournalStats Fabric::journal_stats() const {
  locks::SharedLock guard(mutex_);
  if (journal_ == nullptr) {
    return JournalStats{};
  }
  return journal_->stats();
}

}  // namespace lqf
