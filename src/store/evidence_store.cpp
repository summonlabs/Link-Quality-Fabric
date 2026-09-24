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

#include "lqf/store/evidence_store.hpp"

#include <algorithm>
#include <set>

#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"

namespace lqf {
namespace {

i64 axis_time(const EvidenceRecord& record, WindowAxis axis) {
  return axis == WindowAxis::ObservationTime ? record.observation.observed_at.unix_nanos
                                             : record.received.wall_nanos;
}

bool record_matches(const EvidenceRecord& record, const WindowQuery& query) {
  if (query.link.has_value() && !(record.observation.link == *query.link)) {
    return false;
  }
  if (query.metric.has_value() && !(record.observation.metric == *query.metric)) {
    return false;
  }
  if (query.lane.has_value() && !(record.observation.lane == *query.lane)) {
    return false;
  }
  if (query.source.has_value() && !(record.observation.source.id == *query.source)) {
    return false;
  }
  const i64 time = axis_time(record, query.axis);
  return time >= query.from_nanos && time < query.to_nanos;
}

}  // namespace

u64 evidence_digest(const Observation& observation) {
  std::string canonical;
  canonical.append(render_metric_id(observation.metric));
  canonical.push_back('|');
  canonical.append(text::format_u64(observation.lane.is_aggregate()
                                        ? 0xFFFFFFFFFFFFFFFFULL
                                        : observation.lane.value().value()));
  canonical.push_back('|');
  canonical.append(to_string(observation.unit));
  canonical.push_back('|');
  canonical.append(render_reading(observation.reading, observation.unit));
  canonical.push_back('|');
  canonical.append(text::format_i64(observation.observed_at.unix_nanos));
  canonical.push_back('|');
  canonical.append(text::format_u64(observation.authority.value()));
  canonical.push_back('|');
  canonical.append(to_string(observation.provenance.evidence_class));
  return fnv1a64(canonical);
}

bool EvidenceStore::admit_stream(const StreamKey& key) {
  const std::size_t used = streams_per_link_[key.link];
  if (used >= limits_.max_streams_per_link) {
    return false;
  }
  streams_per_link_[key.link] = used + 1;
  return true;
}

Outcome<IngestOutcome> EvidenceStore::append(const Observation& observation,
                                             const ReceiveStamp& received, IngestOrdinal ordinal,
                                             EvidenceOrigin origin,
                                             const CounterContinuityConfig& counter_config) {
  const StreamKey key{observation.link, observation.metric, observation.lane, observation.source};
  auto entry = streams_.find(key);
  if (entry == streams_.end()) {
    if (!admit_stream(key)) {
      return Status::error(StatusCode::LimitExceeded,
                           "stream capacity of " +
                               std::to_string(limits_.max_streams_per_link) +
                               " reached for this link");
    }
    StreamState fresh;
    fresh.key = key;
    entry = streams_.emplace(key, std::move(fresh)).first;
  }
  StreamState& stream = entry->second;

  const u64 sequence = observation.sequence.value();
  const u64 digest = evidence_digest(observation);
  IngestOutcome outcome;

  if (stream.has_watermark) {
    const auto duplicate = stream.dedup.find(sequence);
    if (duplicate != stream.dedup.end()) {
      if (duplicate->second == digest) {
        stream.stats.duplicates += 1;
        outcome.duplicate = true;
        outcome.reason = ReasonCode::None;
        outcome.stream_depth = stream.recent.size();
        return outcome;
      }
      stream.stats.rejected += 1;
      return Status::error(StatusCode::IdMismatch,
                           "sequence " + text::format_u64(sequence) +
                               " was already accepted with different content");
    }
    if (sequence < stream.watermark) {
      const u64 distance = stream.watermark - sequence;
      if (distance > static_cast<u64>(limits_.max_dedup_window)) {
        stream.stats.rejected += 1;
        return Status::error(StatusCode::Fenced,
                             "sequence " + text::format_u64(sequence) +
                                 " is older than the verification window of " +
                                 text::format_u64(limits_.max_dedup_window));
      }
      stream.stats.reordered += 1;
      outcome.reordered = true;
    }
  }

  EvidenceRecord record;
  record.observation = observation;
  record.received = received;
  record.ordinal = ordinal;
  record.origin = origin;

  stream.dedup.emplace(sequence, digest);
  if (!stream.has_watermark || sequence > stream.watermark) {
    stream.watermark = sequence;
    stream.has_watermark = true;
  }
  // Fence by sequence range, not by insertion order, so a late sample cannot
  // push a newer sequence out of the verification window.
  while (!stream.dedup.empty()) {
    const u64 oldest = stream.dedup.begin()->first;
    if (stream.watermark >= oldest &&
        stream.watermark - oldest >= static_cast<u64>(limits_.max_dedup_window)) {
      stream.dedup.erase(stream.dedup.begin());
    } else {
      break;
    }
  }

  const bool feed = !stream.has_fed_sequence || sequence > stream.fed_sequence;
  if (is_counter(observation.reading)) {
    const auto* counter = as_counter(observation.reading);
    if (counter != nullptr && feed) {
      CounterObservation counter_observation;
      counter_observation.value = counter->value;
      counter_observation.width = counter->width;
      counter_observation.reset_declared = counter->reset_declared;
      counter_observation.observed_nanos = observation.observed_at.unix_nanos;
      counter_observation.received_steady_nanos = received.steady_nanos;
      counter_observation.sequence = observation.sequence;
      counter_observation.incarnation = observation.source.incarnation;
      counter_observation.link_generation = observation.link.generation;
      const CounterStep step = stream.counter.observe(counter_observation, counter_config);
      stream.last_counter_event = step.event;
      stream.last_counter_reason = step.reason;
      stream.last_delta = step.delta;
      stream.rate_admissible = stream.counter.rate_admissible();
      outcome.counter_event = step.event;
      outcome.continuity_broken = !counter_event_preserves_continuity(step.event);
      if (outcome.continuity_broken) {
        stream.stats.continuity_breaks += 1;
      }
    }
  }

  if (feed) {
    stream.latest_fed = record;
    stream.fed_sequence = sequence;
    stream.has_fed_sequence = true;
  }

  stream.recent.push_back(record);
  while (stream.recent.size() > limits_.max_stream_history) {
    stream.recent.pop_front();
    stream.stats.evicted += 1;
    outcome.evicted += 1;
  }
  stream.stats.accepted += 1;
  history_.push_back(record);
  while (history_.size() > limits_.max_global_history) {
    history_.pop_front();
  }
  outcome.accepted = true;
  outcome.rate_admissible = stream.rate_admissible;
  outcome.stream_depth = stream.recent.size();
  return outcome;
}

const StreamState* EvidenceStore::find(const StreamKey& key) const {
  const auto entry = streams_.find(key);
  return entry == streams_.end() ? nullptr : &entry->second;
}

std::vector<const StreamState*> EvidenceStore::streams_for(const LinkIdentity& link,
                                                           const MetricId& metric) const {
  std::vector<const StreamState*> result;
  for (const auto& entry : streams_) {
    if (entry.first.link == link && entry.first.metric == metric) {
      result.push_back(&entry.second);
    }
  }
  return result;
}

std::vector<const StreamState*> EvidenceStore::streams_for_link(const LinkIdentity& link) const {
  std::vector<const StreamState*> result;
  for (const auto& entry : streams_) {
    if (entry.first.link == link) {
      result.push_back(&entry.second);
    }
  }
  return result;
}

EvidenceStore::WindowScan EvidenceStore::window(const WindowQuery& query) const {
  WindowScan scan;
  for (const EvidenceRecord& record : history_) {
    const i64 time = axis_time(record, query.axis);
    if (!scan.has_range) {
      scan.earliest = time;
      scan.latest = time;
      scan.has_range = true;
    } else {
      scan.earliest = std::min(scan.earliest, time);
      scan.latest = std::max(scan.latest, time);
    }
    if (!record_matches(record, query)) {
      continue;
    }
    scan.matched += 1;
    if (scan.records.size() < query.limit) {
      scan.records.push_back(record);
    } else {
      scan.truncated = true;
    }
  }
  std::sort(scan.records.begin(), scan.records.end(),
            [&query](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
              const i64 left = axis_time(lhs, query.axis);
              const i64 right = axis_time(rhs, query.axis);
              if (left != right) {
                return left < right;
              }
              return lhs.ordinal < rhs.ordinal;
            });
  return scan;
}

std::vector<EvidenceRecord> EvidenceStore::retained_records() const {
  std::vector<EvidenceRecord> result;
  for (const auto& entry : streams_) {
    const StreamState& stream = entry.second;
    std::set<u64> emitted;
    if (stream.latest_fed.has_value()) {
      result.push_back(*stream.latest_fed);
      emitted.insert(stream.latest_fed->observation.sequence.value());
    }
    for (const EvidenceRecord& record : stream.recent) {
      if (emitted.insert(record.observation.sequence.value()).second) {
        result.push_back(record);
      }
    }
  }
  std::sort(result.begin(), result.end(), [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
    if (lhs.observation.link != rhs.observation.link) {
      return lhs.observation.link < rhs.observation.link;
    }
    if (lhs.observation.metric != rhs.observation.metric) {
      return lhs.observation.metric < rhs.observation.metric;
    }
    if (lhs.observation.lane != rhs.observation.lane) {
      return lhs.observation.lane < rhs.observation.lane;
    }
    if (lhs.observation.source != rhs.observation.source) {
      return lhs.observation.source < rhs.observation.source;
    }
    return lhs.observation.sequence < rhs.observation.sequence;
  });
  return result;
}

std::vector<std::pair<u64, u64>> EvidenceStore::dedup_window(const StreamKey& key) const {
  const auto entry = streams_.find(key);
  if (entry == streams_.end()) {
    return {};
  }
  std::vector<std::pair<u64, u64>> entries;
  entries.reserve(entry->second.dedup.size());
  for (const auto& item : entry->second.dedup) {
    entries.emplace_back(item.first, item.second);
  }
  return entries;
}

Status EvidenceStore::seed_dedup_window(const StreamKey& key,
                                        const std::vector<std::pair<u64, u64>>& entries) {
  auto entry = streams_.find(key);
  if (entry == streams_.end()) {
    if (!admit_stream(key)) {
      return Status::error(StatusCode::LimitExceeded, "stream capacity reached for this link");
    }
    StreamState fresh;
    fresh.key = key;
    entry = streams_.emplace(key, std::move(fresh)).first;
  }
  StreamState& stream = entry->second;
  for (const auto& item : entries) {
    stream.dedup[item.first] = item.second;
    if (!stream.has_watermark || item.first > stream.watermark) {
      stream.watermark = item.first;
      stream.has_watermark = true;
    }
  }
  while (stream.dedup.size() > limits_.max_dedup_window) {
    stream.dedup.erase(stream.dedup.begin());
  }
  return Status::success();
}

std::vector<StreamKey> EvidenceStore::stream_keys() const {
  std::vector<StreamKey> keys;
  keys.reserve(streams_.size());
  for (const auto& entry : streams_) {
    keys.push_back(entry.first);
  }
  return keys;
}

std::size_t EvidenceStore::drop_link(const LinkIdentity& link) {
  std::size_t dropped = 0;
  for (auto entry = streams_.begin(); entry != streams_.end();) {
    if (entry->first.link == link) {
      entry = streams_.erase(entry);
      dropped += 1;
    } else {
      ++entry;
    }
  }
  streams_per_link_.erase(link);
  std::deque<EvidenceRecord> kept;
  for (const EvidenceRecord& record : history_) {
    if (!(record.observation.link == link)) {
      kept.push_back(record);
    }
  }
  history_.swap(kept);
  return dropped;
}

void EvidenceStore::clear() noexcept {
  streams_.clear();
  history_.clear();
  streams_per_link_.clear();
}

}  // namespace lqf
