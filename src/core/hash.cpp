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

#include "lqf/core/hash.hpp"

#include <cstring>

#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct Crc32cTable {
  std::array<u32, 256> entries{};

  Crc32cTable() {
    constexpr u32 kPolynomial = 0x82F63B78U;
    for (u32 index = 0; index < 256; ++index) {
      u32 value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0U ? (value >> 1U) ^ kPolynomial : (value >> 1U);
      }
      entries[index] = value;
    }
  }
};

const Crc32cTable& crc_table() {
  static const Crc32cTable table;
  return table;
}

constexpr std::array<u32, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr u32 rotr(u32 value, u32 bits) { return (value >> bits) | (value << (32U - bits)); }

}  // namespace

u32 crc32c(const void* data, std::size_t size, u32 seed) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  u32 crc = ~seed;
  const Crc32cTable& table = crc_table();
  for (std::size_t index = 0; index < size; ++index) {
    crc = table.entries[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8U);
  }
  return ~crc;
}

u32 crc32c(std::string_view data, u32 seed) noexcept {
  return crc32c(data.data(), data.size(), seed);
}

u64 fnv1a64(const void* data, std::size_t size, u64 seed) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  u64 hash = seed;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

u64 fnv1a64(std::string_view data, u64 seed) noexcept { return fnv1a64(data.data(), data.size(), seed); }

Sha256::Sha256() noexcept {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256::transform(const unsigned char* block) noexcept {
  u32 words[64];
  for (std::size_t index = 0; index < 16; ++index) {
    words[index] = (static_cast<u32>(block[index * 4]) << 24U) |
                   (static_cast<u32>(block[index * 4 + 1]) << 16U) |
                   (static_cast<u32>(block[index * 4 + 2]) << 8U) |
                   static_cast<u32>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const u32 s0 = rotr(words[index - 15], 7) ^ rotr(words[index - 15], 18) ^ (words[index - 15] >> 3U);
    const u32 s1 = rotr(words[index - 2], 17) ^ rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  u32 a = state_[0];
  u32 b = state_[1];
  u32 c = state_[2];
  u32 d = state_[3];
  u32 e = state_[4];
  u32 f = state_[5];
  u32 g = state_[6];
  u32 h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const u32 choice = (e & f) ^ (~e & g);
    const u32 temp1 = h + s1 + choice + kSha256Constants[index] + words[index];
    const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const u32 majority = (a & b) ^ (a & c) ^ (b & c);
    const u32 temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (finalized_) {
    return;
  }
  const auto* bytes = static_cast<const unsigned char*>(data);
  total_bytes_ += size;
  while (size > 0) {
    const std::size_t room = 64 - buffered_;
    const std::size_t take = size < room ? size : room;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    size -= take;
    if (buffered_ == 64) {
      transform(buffer_.data());
      buffered_ = 0;
    }
  }
}

std::array<unsigned char, Sha256::kDigestBytes> Sha256::finish() noexcept {
  std::array<unsigned char, kDigestBytes> digest{};
  if (!finalized_) {
    const u64 bit_length = total_bytes_ * 8ULL;
    unsigned char padding = 0x80;
    update(&padding, 1);
    padding = 0x00;
    while (buffered_ != 56) {
      update(&padding, 1);
    }
    unsigned char length_bytes[8];
    for (std::size_t index = 0; index < 8; ++index) {
      length_bytes[index] = static_cast<unsigned char>((bit_length >> ((7U - index) * 8U)) & 0xFFU);
    }
    update(length_bytes, 8);
    finalized_ = true;
  }
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<unsigned char>((state_[index] >> 24U) & 0xFFU);
    digest[index * 4 + 1] = static_cast<unsigned char>((state_[index] >> 16U) & 0xFFU);
    digest[index * 4 + 2] = static_cast<unsigned char>((state_[index] >> 8U) & 0xFFU);
    digest[index * 4 + 3] = static_cast<unsigned char>(state_[index] & 0xFFU);
  }
  return digest;
}

std::array<unsigned char, Sha256::kDigestBytes> Sha256::digest(const void* data,
                                                              std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

std::array<unsigned char, Sha256::kDigestBytes> Sha256::digest(std::string_view data) noexcept {
  return digest(data.data(), data.size());
}

std::string Sha256::hex(const std::array<unsigned char, kDigestBytes>& value) {
  return text::hex_bytes(value.data(), value.size());
}

}  // namespace lqf
