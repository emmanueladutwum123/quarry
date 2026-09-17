// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Bit-packing: store `count` integers in `width` bits each instead of 32 or 64.
///
/// This is the primitive the storage layer is built on. Dictionary codes, run
/// lengths and frame-of-reference residuals are all small integers whose range is
/// known before they are written, so paying 32 bits for a value that never exceeds 500
/// wastes 23 bits on every row of the column.
///
/// The layout is little-endian by construction -- values are written LSB-first into a
/// byte stream, not memcpy'd out of a machine word -- so a file written on one
/// architecture reads correctly on another. That is a deliberate cost: a word-at-a-time
/// packer would be faster and would silently produce unreadable files on a big-endian
/// reader.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace quarry::bitpack {

/// Bits needed to represent every value in [0, max_value].
constexpr std::uint32_t width_for(std::uint64_t max_value) {
  std::uint32_t width = 0;
  while (max_value > 0) {
    ++width;
    max_value >>= 1;
  }
  return width;
}

/// Bytes required to hold `count` values of `width` bits.
constexpr std::size_t packed_size(std::size_t count, std::uint32_t width) {
  const std::size_t bits = count * static_cast<std::size_t>(width);
  return (bits + 7) / 8;
}

/// Pack `count` values, each already masked to `width` bits or narrower.
///
/// `width == 0` is legal and writes nothing: a column whose values are all identical
/// carries its single value in the zone map and needs no payload at all.
inline void pack(const std::uint64_t* values, std::size_t count, std::uint32_t width,
                 std::byte* out) {
  if (width == 0 || count == 0) return;
  std::memset(out, 0, packed_size(count, width));

  std::size_t bit = 0;
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t value = values[i];
    if (width < 64) value &= (std::uint64_t{1} << width) - 1;

    std::size_t byte = bit >> 3;
    std::uint32_t offset = static_cast<std::uint32_t>(bit & 7);
    std::uint32_t remaining = width;
    while (remaining > 0) {
      const std::uint32_t take = std::min(8u - offset, remaining);
      const std::uint64_t mask = (std::uint64_t{1} << take) - 1;
      const auto chunk = static_cast<std::uint8_t>((value & mask) << offset);
      out[byte] = static_cast<std::byte>(static_cast<std::uint8_t>(out[byte]) | chunk);
      value >>= take;
      remaining -= take;
      ++byte;
      offset = 0;
    }
    bit += width;
  }
}

/// Unpack `count` values of `width` bits into `out`.
inline void unpack(const std::byte* in, std::size_t count, std::uint32_t width,
                   std::uint64_t* out) {
  // An empty column is a real case -- a row group can be flushed with zero rows in a
  // column, and `vector<uint64_t>(0).data()` is null. Passing a null pointer to
  // memset or memcpy is undefined even when the length is zero, so the early return
  // is load-bearing rather than an optimisation.
  if (count == 0) return;
  if (width == 0) {
    std::memset(out, 0, count * sizeof(std::uint64_t));
    return;
  }
  if constexpr (std::endian::native == std::endian::little) {
    // The byte-aligned widths are exactly the ones a dictionary column lands on most
    // often, and for those the generic bit loop is pure overhead. Guarded on
    // endianness because this path reinterprets file bytes as a host word: on a
    // big-endian machine it would read every value byte-reversed, so that machine
    // takes the slow path and still gets the right answer.
    if (width == 8 || width == 16 || width == 32 || width == 64) {
      const std::size_t bytes = width / 8;
      for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t value = 0;
        std::memcpy(&value, in + i * bytes, bytes);
        out[i] = value;
      }
      return;
    }
  }

  std::size_t bit = 0;
  for (std::size_t i = 0; i < count; ++i) {
    std::uint64_t value = 0;
    std::size_t byte = bit >> 3;
    std::uint32_t offset = static_cast<std::uint32_t>(bit & 7);
    std::uint32_t produced = 0;
    while (produced < width) {
      const std::uint32_t take = std::min(8u - offset, width - produced);
      const std::uint32_t mask = (1u << take) - 1;
      const std::uint32_t chunk =
          (static_cast<std::uint32_t>(in[byte]) >> offset) & mask;
      value |= static_cast<std::uint64_t>(chunk) << produced;
      produced += take;
      ++byte;
      offset = 0;
    }
    out[i] = value;
    bit += width;
  }
}

}  // namespace quarry::bitpack
