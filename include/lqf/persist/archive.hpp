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

#ifndef LQF_PERSIST_ARCHIVE_HPP
#define LQF_PERSIST_ARCHIVE_HPP

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/strong.hpp"
#include "lqf/export.hpp"

namespace lqf {

// Hard bounds for every decoder. A decoder validates a length before it
// allocates and refuses to continue once a bound is exceeded, so a malformed
// stream can never turn into an attacker-chosen allocation.
struct CodecLimits {
  // Identity fields are bounded separately at validation time; this bound also
  // has to fit rendered explanations, which are legitimately long.
  std::size_t max_string_bytes{4096};
  std::size_t max_items{4096};
  std::size_t max_total_bytes{8U << 20U};
};

class LQF_API Writer {
 public:
  explicit Writer(CodecLimits limits = {}) : limits_(limits) {}

  void raw(const void* data, std::size_t size);
  void put_u8(u8 value) { raw(&value, 1); }
  void put_u16(u16 value);
  void put_u32(u32 value);
  void put_u64(u64 value);
  void put_i64(i64 value);
  void put_f64(double value);
  void put_bytes(std::string_view value);
  void put_text(const std::string& value, std::size_t max_bytes);
  void fail(StatusCode code, std::string message);

  [[nodiscard]] const std::string& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::string release() { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] const CodecLimits& limits() const noexcept { return limits_; }
  void clear() noexcept {
    buffer_.clear();
    status_ = Status::success();
  }

 private:
  std::string buffer_{};
  CodecLimits limits_{};
  Status status_{};
};

class LQF_API Reader {
 public:
  Reader(std::string_view data, CodecLimits limits = {}) : data_(data), limits_(limits) {}

  bool raw(void* out, std::size_t size);
  bool get_u8(u8& out) { return raw(&out, 1); }
  bool get_u16(u16& out);
  bool get_u32(u32& out);
  bool get_u64(u64& out);
  bool get_i64(i64& out);
  bool get_f64(double& out);
  bool get_bytes(std::string& out, std::size_t size);
  bool get_text(std::string& out, std::size_t max_bytes);
  void fail(StatusCode code, std::string message);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] bool empty() const noexcept { return offset_ >= data_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] const CodecLimits& limits() const noexcept { return limits_; }

 private:
  std::string_view data_{};
  std::size_t offset_{0};
  CodecLimits limits_{};
  Status status_{};
};

// Decoder bound for enums that cross a boundary. A value outside the declared
// range is refused instead of becoming an unnamed state that later code
// silently treats as the first case. Specialisations live next to the type
// definitions; the primary template accepts only the underlying type bound.
template <class T>
struct enum_bound {
  static constexpr bool known = false;
  static constexpr u64 max_value = 0;
};

namespace detail {

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template <class T>
struct is_pair : std::false_type {};
template <class A, class B>
struct is_pair<std::pair<A, B>> : std::true_type {};

template <class T>
struct is_variant : std::false_type {};
template <class... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

template <class T>
struct is_strong_value : std::false_type {};
template <class Tag, class U>
struct is_strong_value<StrongValue<Tag, U>> : std::true_type {};

template <class T>
struct is_strong_counter : std::false_type {};
template <class Tag>
struct is_strong_counter<StrongCounter<Tag>> : std::true_type {};

}  // namespace detail

class LQF_API Encoder {
 public:
  Encoder(Writer& writer, CodecLimits limits) : writer_(writer), limits_(limits) {}

  template <class T>
  void field(const T& value);

  [[nodiscard]] bool ok() const noexcept { return writer_.ok(); }

 private:
  Writer& writer_;
  CodecLimits limits_;
};

class LQF_API Decoder {
 public:
  Decoder(Reader& reader, CodecLimits limits) : reader_(reader), limits_(limits) {}

  template <class T>
  void field(T& value);

  [[nodiscard]] bool ok() const noexcept { return reader_.ok(); }

 private:
  Reader& reader_;
  CodecLimits limits_;
};

template <class DecoderT, std::size_t Index, class Variant>
void decode_variant(DecoderT& decoder, u8 index, Variant& value) {
  if constexpr (Index < std::variant_size_v<Variant>) {
    if (index == Index) {
      std::variant_alternative_t<Index, Variant> alternative{};
      decoder.field(alternative);
      if (decoder.ok()) {
        value = std::move(alternative);
      }
      return;
    }
    decode_variant<DecoderT, Index + 1>(decoder, index, value);
  } else {
    (void)decoder;
    (void)index;
    (void)value;
  }
}

template <class T>
void Encoder::field(const T& value) {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    writer_.put_u8(value ? u8{1} : u8{0});
  } else if constexpr (std::is_enum_v<U>) {
    using Underlying = std::underlying_type_t<U>;
    if constexpr (sizeof(Underlying) == 1) {
      writer_.put_u8(static_cast<u8>(value));
    } else if constexpr (sizeof(Underlying) == 2) {
      writer_.put_u16(static_cast<u16>(value));
    } else if constexpr (sizeof(Underlying) == 4) {
      writer_.put_u32(static_cast<u32>(value));
    } else {
      writer_.put_u64(static_cast<u64>(value));
    }
  } else if constexpr (std::is_same_v<U, u8>) {
    writer_.put_u8(value);
  } else if constexpr (std::is_same_v<U, u16>) {
    writer_.put_u16(value);
  } else if constexpr (std::is_same_v<U, u32>) {
    writer_.put_u32(value);
  } else if constexpr (std::is_same_v<U, u64>) {
    writer_.put_u64(value);
  } else if constexpr (std::is_same_v<U, i32>) {
    writer_.put_i64(static_cast<i64>(value));
  } else if constexpr (std::is_same_v<U, i64>) {
    writer_.put_i64(value);
  } else if constexpr (std::is_same_v<U, double>) {
    writer_.put_f64(value);
  } else if constexpr (std::is_same_v<U, std::string>) {
    writer_.put_text(value, limits_.max_string_bytes);
  } else if constexpr (detail::is_strong_value<U>::value) {
    field(value.value());
  } else if constexpr (detail::is_strong_counter<U>::value) {
    writer_.put_u64(value.value());
  } else if constexpr (detail::is_optional<U>::value) {
    if (value.has_value()) {
      writer_.put_u8(u8{1});
      field(*value);
    } else {
      writer_.put_u8(u8{0});
    }
  } else if constexpr (detail::is_pair<U>::value) {
    field(value.first);
    field(value.second);
  } else if constexpr (detail::is_vector<U>::value) {
    if (value.size() > limits_.max_items) {
      writer_.fail(StatusCode::LimitExceeded, "item count exceeds the encoder limit");
      return;
    }
    writer_.put_u32(static_cast<u32>(value.size()));
    for (const auto& element : value) {
      field(element);
    }
  } else if constexpr (detail::is_variant<U>::value) {
    writer_.put_u8(static_cast<u8>(value.index()));
    std::visit([this](const auto& alternative) { this->field(alternative); }, value);
  } else {
    // The encoder only reads, so the const view is cast away to reach the one
    // visit_fields overload per type. There is exactly one description of each
    // type for both directions, which is what keeps them from drifting.
    visit_fields(*this, const_cast<std::remove_const_t<T>&>(value));
  }
}

template <class T>
void Decoder::field(T& value) {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>) {
    u8 raw_value = 0;
    if (!reader_.get_u8(raw_value)) {
      return;
    }
    if (raw_value > 1) {
      reader_.fail(StatusCode::Protocol, "boolean field is not 0 or 1");
      return;
    }
    value = raw_value == 1;
  } else if constexpr (std::is_enum_v<U>) {
    using Underlying = std::underlying_type_t<U>;
    u64 raw_value = 0;
    if constexpr (sizeof(Underlying) == 1) {
      u8 narrow = 0;
      if (!reader_.get_u8(narrow)) {
        return;
      }
      raw_value = narrow;
    } else if constexpr (sizeof(Underlying) == 2) {
      u16 narrow = 0;
      if (!reader_.get_u16(narrow)) {
        return;
      }
      raw_value = narrow;
    } else if constexpr (sizeof(Underlying) == 4) {
      u32 narrow = 0;
      if (!reader_.get_u32(narrow)) {
        return;
      }
      raw_value = narrow;
    } else if (!reader_.get_u64(raw_value)) {
      return;
    }
    if (raw_value > static_cast<u64>(std::numeric_limits<Underlying>::max())) {
      reader_.fail(StatusCode::Protocol, "enum value out of range");
      return;
    }
    if (enum_bound<U>::known && raw_value > enum_bound<U>::max_value) {
      reader_.fail(StatusCode::Protocol, "enum value is outside the declared range");
      return;
    }
    value = static_cast<U>(static_cast<Underlying>(raw_value));
  } else if constexpr (std::is_same_v<U, u8>) {
    reader_.get_u8(value);
  } else if constexpr (std::is_same_v<U, u16>) {
    reader_.get_u16(value);
  } else if constexpr (std::is_same_v<U, u32>) {
    reader_.get_u32(value);
  } else if constexpr (std::is_same_v<U, u64>) {
    reader_.get_u64(value);
  } else if constexpr (std::is_same_v<U, i32>) {
    i64 wide = 0;
    if (!reader_.get_i64(wide)) {
      return;
    }
    if (wide < static_cast<i64>(std::numeric_limits<i32>::min()) ||
        wide > static_cast<i64>(std::numeric_limits<i32>::max())) {
      reader_.fail(StatusCode::Protocol, "integer does not fit the target width");
      return;
    }
    value = static_cast<i32>(wide);
  } else if constexpr (std::is_same_v<U, i64>) {
    reader_.get_i64(value);
  } else if constexpr (std::is_same_v<U, double>) {
    reader_.get_f64(value);
  } else if constexpr (std::is_same_v<U, std::string>) {
    reader_.get_text(value, limits_.max_string_bytes);
  } else if constexpr (detail::is_strong_value<U>::value) {
    typename U::value_type inner{};
    field(inner);
    if (reader_.ok()) {
      value = U(std::move(inner));
    }
  } else if constexpr (detail::is_strong_counter<U>::value) {
    u64 raw_value = 0;
    if (reader_.get_u64(raw_value)) {
      value = U(raw_value);
    }
  } else if constexpr (detail::is_optional<U>::value) {
    u8 present = 0;
    if (!reader_.get_u8(present)) {
      return;
    }
    if (present == 0) {
      value.reset();
      return;
    }
    if (present > 1) {
      reader_.fail(StatusCode::Protocol, "optional presence flag is not 0 or 1");
      return;
    }
    typename U::value_type element{};
    field(element);
    if (reader_.ok()) {
      value = std::move(element);
    }
  } else if constexpr (detail::is_pair<U>::value) {
    field(value.first);
    field(value.second);
  } else if constexpr (detail::is_vector<U>::value) {
    u32 count = 0;
    if (!reader_.get_u32(count)) {
      return;
    }
    if (count > limits_.max_items) {
      reader_.fail(StatusCode::LimitExceeded, "item count exceeds the decoder limit");
      return;
    }
    // Every element occupies at least one byte, so a count larger than the
    // remaining input is malformed: validate before allocating.
    if (count > reader_.remaining()) {
      reader_.fail(StatusCode::Protocol, "item count exceeds the remaining input");
      return;
    }
    value.clear();
    value.resize(count);
    for (auto& element : value) {
      field(element);
      if (!reader_.ok()) {
        return;
      }
    }
  } else if constexpr (detail::is_variant<U>::value) {
    u8 index = 0;
    if (!reader_.get_u8(index)) {
      return;
    }
    if (index >= std::variant_size_v<U>) {
      reader_.fail(StatusCode::Protocol, "variant index out of range");
      return;
    }
    decode_variant<Decoder, 0>(*this, index, value);
  } else {
    visit_fields(*this, value);
  }
}

// ADL hook: one overload per persisted type, declared in lqf::codec.
template <class Archive, class T>
void visit_fields(Archive& archive, T& value);

}  // namespace lqf

#endif  // LQF_PERSIST_ARCHIVE_HPP