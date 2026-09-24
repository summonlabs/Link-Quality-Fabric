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

#include "lqf/persist/codec.hpp"

#include <cstring>

namespace lqf {
namespace {

void append_le(std::string& buffer, u64 value, std::size_t bytes) {
  for (std::size_t index = 0; index < bytes; ++index) {
    buffer.push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
  }
}

bool read_le(std::string_view data, std::size_t offset, std::size_t bytes, u64& out) {
  if (offset + bytes > data.size()) {
    return false;
  }
  u64 value = 0;
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<u64>(static_cast<unsigned char>(data[offset + index])) << (index * 8U);
  }
  out = value;
  return true;
}

}  // namespace

void Writer::raw(const void* data, std::size_t size) {
  if (!status_.ok()) {
    return;
  }
  std::size_t total = 0;
  if (!checked_add_size(buffer_.size(), size, total) || total > limits_.max_total_bytes) {
    status_ = Status::error(StatusCode::LimitExceeded, "encoded message exceeds the byte limit");
    return;
  }
  if (size != 0 && data == nullptr) {
    status_ = Status::error(StatusCode::Internal, "null source for a non-empty write");
    return;
  }
  buffer_.append(static_cast<const char*>(data), size);
}

void Writer::put_u16(u16 value) { append_le(buffer_, value, 2); }
void Writer::put_u32(u32 value) { append_le(buffer_, value, 4); }
void Writer::put_u64(u64 value) { append_le(buffer_, value, 8); }
void Writer::put_i64(i64 value) { append_le(buffer_, static_cast<u64>(value), 8); }

void Writer::put_f64(double value) {
  u64 bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  append_le(buffer_, bits, 8);
}

void Writer::put_bytes(std::string_view value) {
  if (!ok()) {
    return;
  }
  if (value.size() > limits_.max_total_bytes ||
      value.size() > static_cast<std::size_t>(0xFFFFFFFFULL)) {
    fail(StatusCode::LimitExceeded, "byte string length does not fit the wire format");
    return;
  }
  const u32 length = static_cast<u32>(value.size());
  put_u32(length);
  raw(value.data(), value.size());
}

void Writer::put_text(const std::string& value, std::size_t max_bytes) {
  if (!ok()) {
    return;
  }
  if (value.size() > max_bytes) {
    fail(StatusCode::LimitExceeded, "text exceeds the declared field limit");
    return;
  }
  put_bytes(std::string_view(value));
}

void Writer::fail(StatusCode code, std::string message) {
  if (status_.ok()) {
    status_ = Status::error(code, std::move(message));
  }
}

bool Reader::raw(void* out, std::size_t size) {
  if (!status_.ok()) {
    return false;
  }
  if (size > remaining()) {
    status_ = Status::error(StatusCode::Protocol, "message ended before the field was complete");
    return false;
  }
  if (size != 0 && out == nullptr) {
    status_ = Status::error(StatusCode::Internal, "null destination for a non-empty read");
    return false;
  }
  std::memcpy(out, data_.data() + offset_, size);
  offset_ += size;
  return true;
}

bool Reader::get_u16(u16& out) {
  u64 value = 0;
  if (!read_le(data_, offset_, 2, value)) {
    if (status_.ok()) {
      status_ = Status::error(StatusCode::Protocol, "message ended before the field was complete");
    }
    return false;
  }
  offset_ += 2;
  out = static_cast<u16>(value);
  return true;
}

bool Reader::get_u32(u32& out) {
  u64 value = 0;
  if (!read_le(data_, offset_, 4, value)) {
    if (status_.ok()) {
      status_ = Status::error(StatusCode::Protocol, "message ended before the field was complete");
    }
    return false;
  }
  offset_ += 4;
  out = static_cast<u32>(value);
  return true;
}

bool Reader::get_u64(u64& out) {
  if (!read_le(data_, offset_, 8, out)) {
    if (status_.ok()) {
      status_ = Status::error(StatusCode::Protocol, "message ended before the field was complete");
    }
    return false;
  }
  offset_ += 8;
  return true;
}

bool Reader::get_i64(i64& out) {
  u64 raw_value = 0;
  if (!get_u64(raw_value)) {
    return false;
  }
  std::memcpy(&out, &raw_value, sizeof(out));
  return true;
}

bool Reader::get_f64(double& out) {
  u64 bits = 0;
  if (!get_u64(bits)) {
    return false;
  }
  std::memcpy(&out, &bits, sizeof(out));
  return true;
}

bool Reader::get_bytes(std::string& out, std::size_t size) {
  if (!status_.ok()) {
    return false;
  }
  if (size > limits_.max_total_bytes) {
    status_ = Status::error(StatusCode::LimitExceeded, "byte string exceeds the decoder limit");
    return false;
  }
  if (size > remaining()) {
    status_ = Status::error(StatusCode::Protocol, "byte string length exceeds the remaining input");
    return false;
  }
  out.assign(data_.data() + offset_, size);
  offset_ += size;
  return true;
}

bool Reader::get_text(std::string& out, std::size_t max_bytes) {
  u32 length = 0;
  if (!get_u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    status_ = Status::error(StatusCode::LimitExceeded, "text field exceeds the declared limit");
    return false;
  }
  return get_bytes(out, length);
}

void Reader::fail(StatusCode code, std::string message) {
  if (status_.ok()) {
    status_ = Status::error(code, std::move(message));
  }
}

CodecLimits codec_limits_for(const FabricLimits& limits) {
  CodecLimits codec;
  // Identity strings are validated against limits.max_string_bytes before they
  // are stored; the codec bound also carries rendered text and provenance.
  codec.max_string_bytes = limits.max_string_bytes > 4096 ? limits.max_string_bytes : 4096;
  codec.max_items = limits.max_evidence_batch > limits.max_metrics_per_query
                        ? limits.max_evidence_batch
                        : limits.max_metrics_per_query;
  if (codec.max_items < limits.max_global_history) {
    codec.max_items = limits.max_global_history;
  }
  codec.max_total_bytes = 8U << 20U;
  return codec;
}

}  // namespace lqf