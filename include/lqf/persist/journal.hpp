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

#ifndef LQF_PERSIST_JOURNAL_HPP
#define LQF_PERSIST_JOURNAL_HPP

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lqf/core/clock.hpp"
#include "lqf/core/lock_tracker.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/export.hpp"
#include "lqf/runtime/config.hpp"

namespace lqf {

enum class JournalRecordKind : u8 {
  Capability = 1,
  Policy = 2,
  LinkObserved = 3,
  LinkGenerationAdvanced = 4,
  Evidence = 5,
  Snapshot = 6,
  // Replay fence state: the sequence to digest window of one stream. Persisted
  // so that a replayed frame is refused as a duplicate after a restart exactly
  // as it was refused before the restart.
  DedupWindow = 7,
  Count,
};

LQF_API const char* to_string(JournalRecordKind kind) noexcept;
LQF_API Status parse_journal_record_kind(u8 raw, JournalRecordKind& out);

struct JournalRecord {
  JournalRecordKind kind{JournalRecordKind::Evidence};
  IngestOrdinal ordinal{};
  std::string body{};
};

// What recovery found. Never optimistic: a torn tail is reported, a degraded
// read stops the replay, and the counters say exactly how much was trusted.
struct RecoveryReport {
  bool opened{false};
  bool header_valid{false};
  bool torn_tail{false};
  bool degraded{false};
  bool truncated_file{false};
  bool temp_discarded{false};
  bool snapshot_loaded{false};
  bool limit_reached{false};
  u64 records_read{0};
  u64 bytes_read{0};
  u64 bytes_discarded{0};
  u64 first_bad_offset{0};
  std::string detail{};
  Status status{};
};

// A snapshot body is itself a bundle of records, so recovery has exactly one
// apply path and compaction cannot diverge from replay.
LQF_API Status encode_record_bundle(const std::vector<JournalRecord>& records, std::string& out,
                                  std::size_t max_bytes);
LQF_API Status decode_record_bundle(std::string_view body, std::vector<JournalRecord>& out,
                                  std::size_t max_records, std::size_t max_bytes);

class LQF_API JournalReader {
 public:
  JournalReader() = default;
  ~JournalReader();
  JournalReader(const JournalReader&) = delete;
  JournalReader& operator=(const JournalReader&) = delete;

  Outcome<RecoveryReport> open(const std::string& path, const JournalConfig& config);
  Outcome<bool> next(JournalRecord& out);
  // Conservative repair: only a torn tail (a short read at end of file) is
  // truncated. A checksum failure anywhere else leaves the file untouched and
  // requires an explicit operator decision.
  Status repair_torn_tail();
  void close();
  [[nodiscard]] const RecoveryReport& report() const noexcept { return report_; }
  // The ordinal the next appended record must carry. A runtime that continues a
  // recovered journal numbers its records from here.
  [[nodiscard]] u64 next_ordinal() const noexcept { return expected_ordinal_; }

 private:
  // Scans forward from a damaged record for any record whose length and
  // checksum validate. A damaged record with intact records after it is
  // corruption, not a torn tail, and recovery must not truncate it away.
  [[nodiscard]] bool intact_record_follows(i64 from_offset);
  std::FILE* file_{nullptr};
  std::string path_{};
  RecoveryReport report_{};
  JournalConfig config_{};
  i64 last_good_offset_{0};
  bool have_ordinal_{false};
  u64 expected_ordinal_{0};
  bool reached_end_{false};
};

struct JournalStats {
  bool open{false};
  u64 records_written{0};
  u64 records_dropped{0};
  u64 bytes_written{0};
  u64 flushes{0};
  u64 compactions{0};
  std::size_t queue_depth{0};
  std::size_t queue_high_water{0};
  u64 rejected_appends{0};
};

// Append-only journal with a bounded queue and a single writer thread. The
// queue rejects rather than blocks, so an append never stalls the caller and
// durability is never silently traded for availability.
class LQF_API BackgroundJournal {
 public:
  explicit BackgroundJournal(JournalConfig config);
  ~BackgroundJournal();
  BackgroundJournal(const BackgroundJournal&) = delete;
  BackgroundJournal& operator=(const BackgroundJournal&) = delete;

  Status open(FabricEpoch epoch, const ClockReading& now);
  Status append(JournalRecordKind kind, IngestOrdinal ordinal, std::string body);
  Status flush();
  Status compact(IngestOrdinal snapshot_ordinal, std::string snapshot_body);
  Status stop();
  // Non-blocking admission probe. The fabric serialises ingestion, so this
  // check cannot race with another producer and a refused record never leaves
  // the state ahead of persistence.
  [[nodiscard]] bool has_capacity() const;
  [[nodiscard]] JournalStats stats() const;
  [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }
  [[nodiscard]] u64 file_size_bytes() const;

 private:
  void writer_loop();

  const JournalConfig config_;
  mutable std::mutex queue_mutex_{};
  std::condition_variable queue_cv_{};
  std::condition_variable idle_cv_{};
  std::deque<JournalRecord> queue_{};
  std::FILE* file_{nullptr};
  std::thread writer_{};
  bool open_{false};
  bool stopping_{false};
  bool accepting_{false};
  bool writing_{false};
  bool writer_paused_{false};
  bool failed_{false};
  Status failure_{};
  JournalStats stats_{};
  u64 last_ordinal_written_{0};
  u64 expected_ordinal_{0};
  bool has_expected_ordinal_{false};
  // Enqueue side ordering, guarded by queue_mutex_. A producer must never be
  // compared against the writer's progress, or a fast producer would be refused
  // while its records are still queued.
  u64 last_enqueued_ordinal_{0};
  bool has_enqueued_ordinal_{false};
  // Serialises compaction against appends. It is only ever taken while the
  // fabric state lock is already held, in that one order.
  mutable locks::TrackedMutex compaction_mutex_{"journal_compaction"};
};

LQF_API Status replace_file_atomically(const std::string& temporary_path, const std::string& final_path);
LQF_API bool file_exists(const std::string& path);
LQF_API Status remove_file(const std::string& path);
LQF_API Outcome<u64> file_size_bytes(const std::string& path);

}  // namespace lqf

#endif  // LQF_PERSIST_JOURNAL_HPP
