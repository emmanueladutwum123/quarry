// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Column encodings, and the chooser that decides between them.
///
/// The engine stores encoded data, not compressed data. No LZ4, no zstd, no block
/// compressor anywhere -- and that is a design decision with a reason, not an
/// omission. A general compressor gets a better ratio, but its output has to be
/// inflated in full before a single value can be read, which puts a memcpy of the
/// whole chunk in front of every scan. These encodings are all decodable in place and
/// most of them are *skippable*: a run-length chunk can answer "does any row match
/// x > 5" without materialising a row, and a dictionary chunk can evaluate a string
/// equality against the dictionary once instead of against every row.
///
/// The chooser picks per chunk by computing the encoded size of each candidate and
/// taking the smallest. Estimating is cheap relative to the write, and a heuristic
/// ("strings get dictionary") is wrong exactly where it costs most -- a high-
/// cardinality string column where the dictionary is larger than the data.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "quarry/column.hpp"
#include "quarry/types.hpp"

namespace quarry {

enum class Encoding : std::uint8_t {
  Plain = 0,       ///< values as-is; the floor every other encoding has to beat
  FrameOfRef = 1,  ///< integers as bit-packed offsets from the chunk minimum
  Dictionary = 2,  ///< distinct values once, then bit-packed codes
  Rle = 3,         ///< (value, run length) pairs
};

constexpr const char* encoding_name(Encoding encoding) {
  switch (encoding) {
    case Encoding::Plain: return "PLAIN";
    case Encoding::FrameOfRef: return "FOR";
    case Encoding::Dictionary: return "DICT";
    case Encoding::Rle: return "RLE";
  }
  return "?";
}

/// Encode `column` with a specific encoding. Returns false when the encoding does not
/// apply to the column's type (frame-of-reference on a string, say) -- the chooser
/// relies on that rather than on a table of which pairs are legal.
bool encode_with(const ColumnVector& column, Encoding encoding,
                 std::vector<std::byte>& out);

/// Encoded size in bytes without producing the bytes, for the chooser.
/// Returns SIZE_MAX when the encoding does not apply.
std::size_t estimate_encoded_size(const ColumnVector& column, Encoding encoding);

/// Try every applicable encoding and keep the smallest output.
Encoding encode_best(const ColumnVector& column, std::vector<std::byte>& out);

/// Decode `row_count` values of `type` from `data`.
ColumnVector decode(const std::byte* data, std::size_t bytes, Encoding encoding,
                    TypeId type, std::size_t row_count);

}  // namespace quarry
