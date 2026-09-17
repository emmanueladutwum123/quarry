// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Explicit little-endian load and store for everything that touches the file format.
///
/// The alternative -- memcpy a host integer into the buffer -- is faster to write and
/// produces a file that is silently unreadable on the other byte order. A storage
/// format is the one part of a system whose bugs outlive the process, so the byte
/// order is spelled out rather than inherited from whatever machine wrote it. On a
/// little-endian host every function here compiles to the same load or store the
/// memcpy would have emitted.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace quarry {

inline void store_u16_le(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFF);
  out[1] = static_cast<std::byte>((value >> 8) & 0xFF);
}

inline void store_u32_le(std::byte* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
  }
}

inline void store_u64_le(std::byte* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
  }
}

inline std::uint16_t load_u16_le(const std::byte* in) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    (static_cast<std::uint16_t>(in[1]) << 8));
}

inline std::uint32_t load_u32_le(const std::byte* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

inline std::uint64_t load_u64_le(const std::byte* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

/// Signed integers go through the unsigned path: a right shift of a negative value is
/// implementation-defined before C++20 and sign-extends after it, neither of which is
/// what a serialiser wants. Two's complement is guaranteed by C++20, so the round trip
/// through the unsigned type is exact.
inline void store_i32_le(std::byte* out, std::int32_t value) {
  store_u32_le(out, static_cast<std::uint32_t>(value));
}
inline void store_i64_le(std::byte* out, std::int64_t value) {
  store_u64_le(out, static_cast<std::uint64_t>(value));
}
inline std::int32_t load_i32_le(const std::byte* in) {
  return static_cast<std::int32_t>(load_u32_le(in));
}
inline std::int64_t load_i64_le(const std::byte* in) {
  return static_cast<std::int64_t>(load_u64_le(in));
}

/// Doubles travel as their IEEE-754 bit pattern. Every target the engine builds for
/// uses IEEE-754 binary64, and `-ffast-math` does not change the representation.
inline void store_f64_le(std::byte* out, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  store_u64_le(out, bits);
}
inline double load_f64_le(const std::byte* in) {
  const std::uint64_t bits = load_u64_le(in);
  double value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace quarry
