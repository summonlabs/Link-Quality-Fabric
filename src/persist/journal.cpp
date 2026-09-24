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

#include "lqf/persist/journal.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "lqf/core/hash.hpp"
#include "lqf/core/text.hpp"
#include "lqf/persist/archive.hpp"
#include "lqf/persist/codec.hpp"
#include "lqf/version.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lqf {
namespace {

constexpr char kMagic[8] = {'L', 'Q', 'F', 'J', 'R', 'N', 'L', '1'};
constexpr std::size_t kFileHeaderBytes = 40;
constexpr std::size_t kRecordHeaderBytes = 8;
constexpr std::size_t kRecordPrefixBytes = 9;  // ordinal + kind

void encode_u32(unsigned char* out, u32 value) {
  out[0] = static_cast<unsigned char>(value & 0xFFU);
  out[1] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
  out[2] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
  out[3] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
}

u32 decode_u32(const unsigned char* in) {
  return static_cast<u32>(in[0]) | (static_cast<u32>(in[1]) << 8U) |
         (static_cast<u32>(in[2]) << 16U) | (static_cast<u32>(in[3]) << 24U);
}

void encode_u64(unsigned char* out, u64 value) {
  for (std::size_t index = 0; index < 8; ++index) {
    out[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
  }
}

u64 decode_u64(const unsigned char* in) {
  u64 value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<u64>(in[index]) << (index * 8U);
  }
  return value;
}

std::FILE* open_file(const std::string& path, const char* mode) {
#if defined(_WIN32)
  // fopen_s opens with sharing denied, which would make it impossible to verify
  // or size a journal while the runtime holds it. Sharing is required here.
  return _fsopen(path.c_str(), mode, _SH_DENYNO);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

int seek_file(std::FILE* file, i64 offset, int origin) {
#if defined(_WIN32)
  return _fseeki64(file, offset, origin);
#else
  return fseeko(file, static_cast<off_t>(offset), origin);
#endif
}

i64 tell_file(std::FILE* file) {
#if defined(_WIN32)
  return _ftelli64(file);
#else
  return static_cast<i64>(ftello(file));
#endif
}

Status sync_file(std::FILE* file, bool durable) {
  if (std::fflush(file) != 0) {
    return Status::error(StatusCode::Io, "flush failed");
  }
  if (!durable) {
    return Status::success();
  }
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return Status::error(StatusCode::Io, "commit failed");
  }
#else
  if (fsync(fileno(file)) != 0) {
    return Status::error(StatusCode::Io, "fsync failed");
  }
#endif
  return Status::success();
}

std::string build_file_header(u64 producer_epoch, i64 created_wall_nanos) {
  std::string header(kFileHeaderBytes, '\0');
  std::memcpy(header.data(), kMagic, sizeof(kMagic));
  encode_u32(reinterpret_cast<unsigned char*>(header.data()) + 8, LQF_JOURNAL_FORMAT_VERSION);
  encode_u32(reinterpret_cast<unsigned char*>(header.data()) + 12, 0);
  encode_u64(reinterpret_cast<unsigned char*>(header.data()) + 16, producer_epoch);
  encode_u64(reinterpret_cast<unsigned char*>(header.data()) + 24,
             static_cast<u64>(created_wall_nanos));
  const u32 crc = crc32c(header.data(), 32);
  encode_u32(reinterpret_cast<unsigned char*>(header.data()) + 32, crc);
  encode_u32(reinterpret_cast<unsigned char*>(header.data()) + 36, 0);
  return header;
}

Status validate_file_header(const std::string& header, u64& producer_epoch) {
  if (header.size() != kFileHeaderBytes) {
    return Status::error(StatusCode::Corrupt, "journal header is truncated");
  }
  if (std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0) {
    return Status::error(StatusCode::Corrupt, "journal magic does not match");
  }
  const auto* bytes = reinterpret_cast<const unsigned char*>(header.data());
  const u32 version = decode_u32(bytes + 8);
  if (version != LQF_JOURNAL_FORMAT_VERSION) {
    return Status::error(StatusCode::Unsupported,
                         "journal format version " + text::format_u64(version) +
                             " is not supported by this build");
  }
  if (decode_u32(bytes + 32) != crc32c(header.data(), 32)) {
    return Status::error(StatusCode::Corrupt, "journal header checksum does not match");
  }
  producer_epoch = decode_u64(bytes + 16);
  return Status::success();
}

std::string encode_record_bytes(const JournalRecord& record) {
  std::string payload;
  payload.resize(kRecordPrefixBytes);
  encode_u64(reinterpret_cast<unsigned char*>(payload.data()), record.ordinal.value());
  payload[kRecordPrefixBytes - 1] = static_cast<char>(record.kind);
  payload.append(record.body);

  std::string out;
  out.resize(kRecordHeaderBytes);
  encode_u32(reinterpret_cast<unsigned char*>(out.data()),
             static_cast<u32>(payload.size()));
  encode_u32(reinterpret_cast<unsigned char*>(out.data()) + 4, crc32c(payload));
  out.append(payload);
  return out;
}

}  // namespace

const char* to_string(JournalRecordKind kind) noexcept {
  switch (kind) {
    case JournalRecordKind::Capability: return "capability";
    case JournalRecordKind::Policy: return "policy";
    case JournalRecordKind::LinkObserved: return "link-observed";
    case JournalRecordKind::LinkGenerationAdvanced: return "link-generation-advanced";
    case JournalRecordKind::Evidence: return "evidence";
    case JournalRecordKind::Snapshot: return "snapshot";
    case JournalRecordKind::DedupWindow: return "dedup-window";
    case JournalRecordKind::Count: break;
  }
  return "invalid";
}

Status parse_journal_record_kind(u8 raw, JournalRecordKind& out) {
  if (raw == 0 || raw >= static_cast<u8>(JournalRecordKind::Count)) {
    return Status::error(StatusCode::Protocol, "journal record kind is out of range");
  }
  out = static_cast<JournalRecordKind>(raw);
  return Status::success();
}

Status encode_record_bundle(const std::vector<JournalRecord>& records, std::string& out,
                            std::size_t max_bytes) {
  CodecLimits limits;
  limits.max_items = 1U << 20U;
  limits.max_string_bytes = 1U << 20U;
  limits.max_total_bytes = max_bytes;
  Writer writer(limits);
  if (records.size() > 0xFFFFFFFFULL) {
    return Status::error(StatusCode::LimitExceeded, "record bundle count does not fit");
  }
  writer.put_u32(static_cast<u32>(records.size()));
  for (const JournalRecord& record : records) {
    writer.put_u64(record.ordinal.value());
    writer.put_u8(static_cast<u8>(record.kind));
    writer.put_bytes(record.body);
    if (!writer.ok()) {
      return writer.status();
    }
  }
  if (!writer.ok()) {
    return writer.status();
  }
  out = writer.release();
  return Status::success();
}

Status decode_record_bundle(std::string_view body, std::vector<JournalRecord>& out,
                            std::size_t max_records, std::size_t max_bytes) {
  CodecLimits limits;
  limits.max_items = 1U << 20U;
  limits.max_string_bytes = 1U << 20U;
  limits.max_total_bytes = max_bytes;
  Reader reader(body, limits);
  u32 count = 0;
  if (!reader.get_u32(count)) {
    return reader.status();
  }
  if (count > max_records) {
    return Status::error(StatusCode::LimitExceeded, "record bundle exceeds the record limit");
  }
  out.clear();
  out.reserve(count);
  for (u32 index = 0; index < count; ++index) {
    JournalRecord record;
    u64 ordinal = 0;
    u8 kind = 0;
    if (!reader.get_u64(ordinal) || !reader.get_u8(kind)) {
      return reader.status();
    }
    const Status kind_status = parse_journal_record_kind(kind, record.kind);
    if (!kind_status.ok()) {
      return kind_status;
    }
    record.ordinal = IngestOrdinal(ordinal);
    std::string payload;
    u32 length = 0;
    if (!reader.get_u32(length)) {
      return reader.status();
    }
    if (length > max_bytes) {
      return Status::error(StatusCode::LimitExceeded, "record body exceeds the byte limit");
    }
    if (!reader.get_bytes(payload, length)) {
      return reader.status();
    }
    record.body = std::move(payload);
    out.push_back(std::move(record));
  }
  if (!reader.empty()) {
    return Status::error(StatusCode::Protocol, "record bundle has trailing bytes");
  }
  return Status::success();
}

JournalReader::~JournalReader() { close(); }

Outcome<RecoveryReport> JournalReader::open(const std::string& path, const JournalConfig& config) {
  close();
  path_ = path;
  config_ = config;
  report_ = RecoveryReport{};
  last_good_offset_ = 0;
  have_ordinal_ = false;
  expected_ordinal_ = 0;
  reached_end_ = false;

  const std::string temporary = path + ".tmp";
  if (file_exists(temporary)) {
    const Status removed = remove_file(temporary);
    report_.temp_discarded = true;
    if (!removed.ok()) {
      return Status::error(StatusCode::Io,
                           "an interrupted compaction left a temporary file that could not be "
                           "removed: " + removed.message());
    }
  }

  const bool existed = file_exists(path);
  file_ = open_file(path, existed ? "r+b" : "w+b");
  if (file_ == nullptr) {
    return Status::error(StatusCode::Io, "journal file could not be opened for recovery");
  }
  report_.opened = true;

  std::string header(kFileHeaderBytes, '\0');
  std::size_t read = 0;
  if (existed) {
    read = std::fread(header.data(), 1, kFileHeaderBytes, file_);
  }
  if (!existed || read == 0) {
    // A zero length file is an uninitialised journal, not a corrupt one.
    (void)seek_file(file_, 0, SEEK_SET);
    const std::string fresh = build_file_header(0, 0);
    if (std::fwrite(fresh.data(), 1, fresh.size(), file_) != fresh.size()) {
      return Status::error(StatusCode::Io, "journal header could not be written");
    }
    const Status synced = sync_file(file_, true);
    if (!synced.ok()) {
      return synced;
    }
    report_.header_valid = true;
    last_good_offset_ = static_cast<i64>(kFileHeaderBytes);
    reached_end_ = true;
    return report_;
  }
  if (read < kFileHeaderBytes) {
    report_.torn_tail = true;
    report_.detail = "journal header is truncated";
    report_.bytes_discarded = static_cast<u64>(kFileHeaderBytes - read);
    report_.first_bad_offset = 0;
    return Status::error(StatusCode::Corrupt, "journal header is truncated");
  }
  u64 producer_epoch = 0;
  const Status header_status = validate_file_header(header, producer_epoch);
  if (!header_status.ok()) {
    report_.status = header_status;
    report_.detail = header_status.message();
    return header_status;
  }
  report_.header_valid = true;
  last_good_offset_ = static_cast<i64>(kFileHeaderBytes);
  return report_;
}

Outcome<bool> JournalReader::next(JournalRecord& out) {
  if (file_ == nullptr) {
    return Status::error(StatusCode::Unavailable, "journal reader is not open");
  }
  if (reached_end_) {
    return false;
  }
  if (report_.records_read >= config_.max_recovery_records ||
      report_.bytes_read >= config_.max_recovery_bytes) {
    report_.limit_reached = true;
    report_.detail = "recovery stopped at the configured record or byte limit";
    reached_end_ = true;
    return false;
  }

  unsigned char header[kRecordHeaderBytes];
  const std::size_t got = std::fread(header, 1, kRecordHeaderBytes, file_);
  if (got == 0 && std::feof(file_) != 0) {
    reached_end_ = true;
    return false;
  }
  if (got < kRecordHeaderBytes) {
    report_.torn_tail = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "a record header is truncated at the end of the journal";
    reached_end_ = true;
    return false;
  }
  const u32 payload_length = decode_u32(header);
  const u32 expected_crc = decode_u32(header + 4);
  if (payload_length < kRecordPrefixBytes || payload_length > config_.max_record_bytes) {
    report_.torn_tail = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "a record length is outside the permitted range";
    reached_end_ = true;
    return false;
  }

  std::string payload;
  payload.resize(payload_length);
  const std::size_t payload_read = std::fread(payload.data(), 1, payload_length, file_);
  if (payload_read < payload_length) {
    report_.torn_tail = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "a record body is truncated at the end of the journal";
    reached_end_ = true;
    return false;
  }
  if (crc32c(payload) != expected_crc) {
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    if (intact_record_follows(last_good_offset_ + static_cast<i64>(kRecordHeaderBytes))) {
      // Intact records follow the damaged one: this is corruption inside the
      // journal, not an interrupted append. Nothing is truncated and the runtime
      // refuses to continue on a history it cannot verify.
      report_.degraded = true;
      report_.detail = "a record checksum does not match and intact records follow it";
    } else {
      report_.torn_tail = true;
      report_.detail = "a record checksum does not match at the end of the journal";
    }
    reached_end_ = true;
    return false;
  }

  const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
  const u64 ordinal = decode_u64(bytes);
  JournalRecordKind kind = JournalRecordKind::Evidence;
  const Status kind_status = parse_journal_record_kind(bytes[8], kind);
  if (!kind_status.ok()) {
    report_.degraded = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "a record kind is not recognised; recovery stopped conservatively";
    reached_end_ = true;
    return false;
  }
  if (have_ordinal_ && ordinal <= expected_ordinal_ - 1U) {
    report_.degraded = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "journal ordinals are not strictly increasing; recovery stopped";
    reached_end_ = true;
    return false;
  }
  if (have_ordinal_ && ordinal != expected_ordinal_) {
    report_.degraded = true;
    report_.first_bad_offset = static_cast<u64>(last_good_offset_);
    report_.detail = "journal ordinals have a gap; recovery stopped conservatively";
    reached_end_ = true;
    return false;
  }

  out.kind = kind;
  out.ordinal = IngestOrdinal(ordinal);
  out.body = payload.substr(kRecordPrefixBytes);

  last_good_offset_ += static_cast<i64>(kRecordHeaderBytes + payload_length);
  report_.records_read += 1;
  report_.bytes_read += kRecordHeaderBytes + payload_length;
  have_ordinal_ = true;
  expected_ordinal_ = ordinal + 1U;
  if (kind == JournalRecordKind::Snapshot) {
    report_.snapshot_loaded = true;
  }
  return true;
}

bool JournalReader::intact_record_follows(i64 from_offset) {
  if (file_ == nullptr || from_offset < 0) {
    return false;
  }
  if (seek_file(file_, from_offset, SEEK_SET) != 0) {
    return false;
  }
  std::string buffer;
  const std::size_t chunk = 1U << 16U;
  char raw[1U << 16U];
  std::size_t total = 0;
  while (total < config_.max_recovery_bytes) {
    const std::size_t read = std::fread(raw, 1, chunk, file_);
    if (read == 0) {
      break;
    }
    buffer.append(raw, read);
    total += read;
  }
  if (buffer.size() < kRecordHeaderBytes) {
    return false;
  }
  const auto* bytes = reinterpret_cast<const unsigned char*>(buffer.data());
  for (std::size_t offset = 0; offset + kRecordHeaderBytes <= buffer.size(); ++offset) {
    const u32 length = decode_u32(bytes + offset);
    const u32 crc = decode_u32(bytes + offset + 4);
    if (length < kRecordPrefixBytes || length > config_.max_record_bytes) {
      continue;
    }
    if (offset + kRecordHeaderBytes + length > buffer.size()) {
      continue;
    }
    if (crc32c(std::string_view(buffer.data() + offset + kRecordHeaderBytes, length)) == crc) {
      return true;
    }
  }
  return false;
}

Status JournalReader::repair_torn_tail() {
  if (file_ == nullptr) {
    return Status::error(StatusCode::Unavailable, "journal reader is not open");
  }
  if (!report_.torn_tail) {
    return Status::error(StatusCode::Refused,
                         "the journal tail is intact; no repair is performed");
  }
  Outcome<u64> size = file_size_bytes(path_);
  if (!size.ok()) {
    return size.status();
  }
  const u64 truncated = size.value() > static_cast<u64>(last_good_offset_)
                            ? size.value() - static_cast<u64>(last_good_offset_)
                            : 0;
  if (std::fflush(file_) != 0) {
    return Status::error(StatusCode::Io, "journal flush before repair failed");
  }
#if defined(_WIN32)
  if (_chsize_s(_fileno(file_), last_good_offset_) != 0) {
    return Status::error(StatusCode::Io, "journal could not be truncated to the last good record");
  }
#else
  if (ftruncate(fileno(file_), static_cast<off_t>(last_good_offset_)) != 0) {
    return Status::error(StatusCode::Io, "journal could not be truncated to the last good record");
  }
#endif
  const Status synced = sync_file(file_, true);
  if (!synced.ok()) {
    return synced;
  }
  report_.bytes_discarded = truncated;
  report_.truncated_file = true;
  report_.detail.append(report_.detail.empty() ? "" : "; ");
  report_.detail.append("the damaged tail was truncated to the last verified record");
  return Status::success();
}

void JournalReader::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

BackgroundJournal::BackgroundJournal(JournalConfig config) : config_(std::move(config)) {}

BackgroundJournal::~BackgroundJournal() {
  const Status status = stop();
  (void)status;
}

Status BackgroundJournal::open(FabricEpoch epoch, const ClockReading& now) {
  if (!config_.enabled) {
    return Status::error(StatusCode::Refused, "the journal is disabled by configuration");
  }
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (open_) {
    return Status::error(StatusCode::Busy, "the journal is already open");
  }
  const std::string temporary = config_.path + ".tmp";
  if (file_exists(temporary)) {
    const Status removed = remove_file(temporary);
    if (!removed.ok()) {
      return removed;
    }
  }
  const bool existed = file_exists(config_.path);
  file_ = open_file(config_.path, existed ? "r+b" : "w+b");
  if (file_ == nullptr) {
    return Status::error(StatusCode::Io, "journal file could not be opened for append");
  }
  (void)seek_file(file_, 0, SEEK_END);
  const i64 position = tell_file(file_);
  if (position <= 0) {
    const std::string header = build_file_header(epoch.value(), now.wall_nanos);
    if (std::fwrite(header.data(), 1, header.size(), file_) != header.size()) {
      std::fclose(file_);
      file_ = nullptr;
      return Status::error(StatusCode::Io, "journal header could not be written");
    }
    const Status synced = sync_file(file_, config_.fsync_on_flush);
    if (!synced.ok()) {
      std::fclose(file_);
      file_ = nullptr;
      return synced;
    }
  }
  open_ = true;
  accepting_ = true;
  stopping_ = false;
  failed_ = false;
  failure_ = Status::success();
  stats_.open = true;
  writer_ = std::thread([this]() { writer_loop(); });
  (void)config_.overflow;
  return Status::success();
}

void BackgroundJournal::writer_loop() {
  for (;;) {
    JournalRecord record;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() {
        return (!queue_.empty() && !writer_paused_) || stopping_ || failed_;
      });
      if (writer_paused_) {
        lock.unlock();
        queue_cv_.notify_all();
        continue;
      }
      if (queue_.empty()) {
        if (stopping_ || failed_) {
          break;
        }
        continue;
      }
      record = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
    }

    Status write_status = Status::success();
    if (file_ == nullptr) {
      write_status = Status::error(StatusCode::Unavailable, "journal file is not open");
    } else {
      const std::string encoded = encode_record_bytes(record);
      if (std::fwrite(encoded.data(), 1, encoded.size(), file_) != encoded.size()) {
        write_status = Status::error(StatusCode::Io, "journal record could not be written");
      }
    }

    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      writing_ = false;
      if (write_status.ok()) {
        stats_.records_written += 1;
        stats_.bytes_written += kRecordHeaderBytes + kRecordPrefixBytes + record.body.size();
        last_ordinal_written_ = record.ordinal.value();
        expected_ordinal_ = record.ordinal.value() + 1U;
        has_expected_ordinal_ = true;
      } else {
        failed_ = true;
        failure_ = write_status;
        stats_.records_dropped += queue_.size();
        queue_.clear();
      }
      stats_.queue_depth = queue_.size();
      idle_cv_.notify_all();
      queue_cv_.notify_all();
    }
  }

  if (file_ != nullptr) {
    const Status synced = sync_file(file_, config_.fsync_on_flush);
    if (!synced.ok()) {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      failed_ = true;
      failure_ = synced;
    }
  }
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    writing_ = false;
    stats_.queue_depth = queue_.size();
    idle_cv_.notify_all();
  }
}

Status BackgroundJournal::append(JournalRecordKind kind, IngestOrdinal ordinal, std::string body) {
  if (!config_.enabled) {
    return Status::error(StatusCode::Refused, "the journal is disabled by configuration");
  }
  if (body.size() > config_.max_record_bytes) {
    return Status::error(StatusCode::LimitExceeded, "journal record exceeds the record limit");
  }
  locks::SharedLock compaction(compaction_mutex_);
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (!open_ || !accepting_) {
    return Status::error(StatusCode::Unavailable, "the journal is not accepting records");
  }
  if (failed_) {
    return failure_;
  }
  if (queue_.size() >= config_.max_queue_depth) {
    stats_.rejected_appends += 1;
    return Status::error(StatusCode::Busy, "journal queue is full; the record was refused");
  }
  if (has_enqueued_ordinal_ && ordinal.value() != last_enqueued_ordinal_ + 1U) {
    return Status::error(StatusCode::Internal,
                         "journal ordinals must increase by exactly one; the record was refused");
  }
  last_enqueued_ordinal_ = ordinal.value();
  has_enqueued_ordinal_ = true;
  JournalRecord record;
  record.kind = kind;
  record.ordinal = ordinal;
  record.body = std::move(body);
  queue_.push_back(std::move(record));
  stats_.queue_depth = queue_.size();
  stats_.queue_high_water = std::max(stats_.queue_high_water, stats_.queue_depth);
  lock.unlock();
  queue_cv_.notify_all();
  return Status::success();
}

Status BackgroundJournal::flush() {
  if (!config_.enabled) {
    return Status::error(StatusCode::Refused, "the journal is disabled by configuration");
  }
  locks::SharedLock compaction(compaction_mutex_);
  std::unique_lock<std::mutex> lock(queue_mutex_);
  idle_cv_.wait(lock, [this]() { return (queue_.empty() && !writing_) || failed_ || !open_; });
  if (!open_) {
    return Status::error(StatusCode::Unavailable, "the journal is not open");
  }
  if (failed_) {
    return failure_;
  }
  if (file_ != nullptr) {
    const Status synced = sync_file(file_, config_.fsync_on_flush);
    if (!synced.ok()) {
      failed_ = true;
      failure_ = synced;
      return synced;
    }
  }
  stats_.flushes += 1;
  return Status::success();
}

Status BackgroundJournal::compact(IngestOrdinal snapshot_ordinal, std::string snapshot_body) {
  if (!config_.enabled) {
    return Status::error(StatusCode::Refused, "the journal is disabled by configuration");
  }
  if (snapshot_body.size() > config_.max_record_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "snapshot exceeds the journal record limit; compaction refused");
  }
  locks::UniqueLock compaction(compaction_mutex_);
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    idle_cv_.wait(lock, [this]() { return (queue_.empty() && !writing_) || failed_ || !open_; });
    if (!open_) {
      return Status::error(StatusCode::Unavailable, "the journal is not open");
    }
    if (failed_) {
      return failure_;
    }
    if (has_expected_ordinal_ && expected_ordinal_ > snapshot_ordinal.value() + 1U) {
      return Status::error(StatusCode::Internal,
                           "compaction was requested for an ordinal that is already superseded");
    }
    writer_paused_ = true;
  }

  const std::string temporary = config_.path + ".tmp";
  Status status = Status::success();
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  std::FILE* fresh = open_file(temporary, "wb");
  if (fresh == nullptr) {
    status = Status::error(StatusCode::Io, "compaction temporary file could not be created");
  } else {
    const std::string header = build_file_header(0, 0);
    if (std::fwrite(header.data(), 1, header.size(), fresh) != header.size()) {
      status = Status::error(StatusCode::Io, "compaction header could not be written");
    }
    if (status.ok()) {
      JournalRecord record;
      record.kind = JournalRecordKind::Snapshot;
      record.ordinal = snapshot_ordinal;
      record.body = std::move(snapshot_body);
      const std::string encoded = encode_record_bytes(record);
      if (std::fwrite(encoded.data(), 1, encoded.size(), fresh) != encoded.size()) {
        status = Status::error(StatusCode::Io, "compaction snapshot could not be written");
      }
    }
    if (status.ok()) {
      status = sync_file(fresh, config_.fsync_on_flush);
    }
    std::fclose(fresh);
  }
  if (status.ok()) {
    status = replace_file_atomically(temporary, config_.path);
  }

  if (status.ok()) {
    file_ = open_file(config_.path, "r+b");
    if (file_ == nullptr) {
      status = Status::error(StatusCode::Io, "journal could not be reopened after compaction");
    } else {
      (void)seek_file(file_, 0, SEEK_END);
      stats_.compactions += 1;
      stats_.bytes_written += kFileHeaderBytes + kRecordHeaderBytes + kRecordPrefixBytes;
      last_ordinal_written_ = snapshot_ordinal.value();
      expected_ordinal_ = snapshot_ordinal.value() + 1U;
      has_expected_ordinal_ = true;
      last_enqueued_ordinal_ = snapshot_ordinal.value();
      has_enqueued_ordinal_ = true;
    }
  }
  if (!status.ok() && file_ == nullptr) {
    file_ = open_file(config_.path, "r+b");
    if (file_ == nullptr) {
      failed_ = true;
      failure_ = status;
      open_ = false;
      accepting_ = false;
      stats_.open = false;
    }
  }

  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    writer_paused_ = false;
    stats_.queue_depth = queue_.size();
  }
  queue_cv_.notify_all();
  return status;
}

Status BackgroundJournal::stop() {
  bool should_join = false;
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (open_ || writer_.joinable()) {
      stopping_ = true;
      accepting_ = false;
    }
    should_join = writer_.joinable() && writer_.get_id() != std::this_thread::get_id();
  }
  queue_cv_.notify_all();
  if (should_join) {
    writer_.join();
  }
  std::unique_lock<std::mutex> lock(queue_mutex_);
  Status result = Status::success();
  if (failed_) {
    result = failure_;
  }
  if (file_ != nullptr) {
    const Status synced = sync_file(file_, config_.fsync_on_flush);
    if (!synced.ok() && result.ok()) {
      result = synced;
    }
    std::fclose(file_);
    file_ = nullptr;
  }
  open_ = false;
  accepting_ = false;
  stats_.open = false;
  stats_.queue_depth = queue_.size();
  return result;
}

bool BackgroundJournal::has_capacity() const {
  if (!config_.enabled) {
    return false;
  }
  std::unique_lock<std::mutex> lock(queue_mutex_);
  return open_ && accepting_ && !failed_ && queue_.size() < config_.max_queue_depth;
}

JournalStats BackgroundJournal::stats() const {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  JournalStats copy = stats_;
  copy.queue_depth = queue_.size();
  return copy;
}

u64 BackgroundJournal::file_size_bytes() const {
  Outcome<u64> size = lqf::file_size_bytes(config_.path);
  return size.ok() ? size.value() : 0;
}

Status replace_file_atomically(const std::string& temporary_path, const std::string& final_path) {
#if defined(_WIN32)
  if (MoveFileExA(temporary_path.c_str(), final_path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::error(StatusCode::Io,
                         "atomic replace failed with error " +
                             text::format_u64(static_cast<u64>(GetLastError())));
  }
  return Status::success();
#else
  if (std::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
    return Status::error(StatusCode::Io, "atomic replace failed");
  }
  return Status::success();
#endif
}

bool file_exists(const std::string& path) {
  std::FILE* file = open_file(path, "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

Status remove_file(const std::string& path) {
  if (std::remove(path.c_str()) != 0) {
    return Status::error(StatusCode::Io, "file could not be removed: " + path);
  }
  return Status::success();
}

Outcome<u64> file_size_bytes(const std::string& path) {
  std::FILE* file = open_file(path, "rb");
  if (file == nullptr) {
    return Status::error(StatusCode::NotFound, "file could not be opened: " + path);
  }
#if defined(_WIN32)
  const __int64 size = _filelengthi64(_fileno(file));
  std::fclose(file);
  if (size < 0) {
    return Status::error(StatusCode::Io, "file size could not be read");
  }
  return static_cast<u64>(size);
#else
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return Status::error(StatusCode::Io, "file size could not be read");
  }
  const long size = std::ftell(file);
  std::fclose(file);
  if (size < 0) {
    return Status::error(StatusCode::Io, "file size could not be read");
  }
  return static_cast<u64>(size);
#endif
}

}  // namespace lqf
