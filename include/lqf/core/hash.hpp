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

#ifndef LQF_CORE_HASH_HPP
#define LQF_CORE_HASH_HPP

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "lqf/core/checked.hpp"
#include "lqf/export.hpp"

namespace lqf {

// CRC-32C (Castagnoli), used for journal record integrity and wire framing.
// Hardware acceleration is not assumed; the table driven form is used so the
// result is identical everywhere.
LQF_API u32 crc32c(const void* data, std::size_t size, u32 seed = 0) noexcept;
LQF_API u32 crc32c(std::string_view data, u32 seed = 0) noexcept;

// FNV-1a 64, used for cheap non-cryptographic bucketing and digests.
LQF_API u64 fnv1a64(const void* data, std::size_t size, u64 seed = 0xCBF29CE484222325ULL) noexcept;
LQF_API u64 fnv1a64(std::string_view data, u64 seed = 0xCBF29CE484222325ULL) noexcept;

// SHA-256, used where a content hash must be collision resistant: published
// policy generations and the persisted snapshot digest.
class LQF_API Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;

  Sha256() noexcept;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }
  [[nodiscard]] std::array<unsigned char, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<unsigned char, kDigestBytes> digest(const void* data,
                                                                     std::size_t size) noexcept;
  [[nodiscard]] static std::array<unsigned char, kDigestBytes> digest(std::string_view data) noexcept;
  [[nodiscard]] static std::string hex(const std::array<unsigned char, kDigestBytes>& value);

 private:
  void transform(const unsigned char* block) noexcept;

  std::array<u32, 8> state_{};
  std::array<unsigned char, 64> buffer_{};
  std::size_t buffered_{0};
  u64 total_bytes_{0};
  bool finalized_{false};
};

}  // namespace lqf

#endif  // LQF_CORE_HASH_HPP
