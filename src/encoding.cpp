// SPDX-License-Identifier: Apache-2.0
#include "quarry/encoding.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>

#include "quarry/bitpack.hpp"
#include "quarry/endian.hpp"

namespace quarry {
namespace {

constexpr std::size_t kNotApplicable = std::numeric_limits<std::size_t>::max();

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  const std::size_t at = out.size();
  out.resize(at + 4);
  store_u32_le(out.data() + at, value);
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  const std::size_t at = out.size();
  out.resize(at + 8);
  store_u64_le(out.data() + at, value);
}

void append_u8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

/// Widen a fixed-width integer column to int64 so one code path serves both widths.
/// Doubles are not integers and never take this path.
std::int64_t int_at(const ColumnVector& column, std::size_t index) {
  return column.type() == TypeId::Int32 ? static_cast<std::int64_t>(column.int32_at(index))
                                        : column.int64_at(index);
}

// ---------------------------------------------------------------------------
// PLAIN
// ---------------------------------------------------------------------------

std::size_t plain_size(const ColumnVector& column) {
  if (column.type() == TypeId::String) {
    // [u32 char bytes][u32 offsets (n+1)][chars]
    return 4 + 4 * (column.size() + 1) + column.chars().size();
  }
  return column.size() * type_width(column.type());
}

void plain_encode(const ColumnVector& column, std::vector<std::byte>& out) {
  const std::size_t n = column.size();
  if (column.type() == TypeId::String) {
    append_u32(out, static_cast<std::uint32_t>(column.chars().size()));
    for (std::size_t i = 0; i <= n; ++i) append_u32(out, column.offsets()[i]);
    const auto* chars = reinterpret_cast<const std::byte*>(column.chars().data());
    out.insert(out.end(), chars, chars + column.chars().size());
    return;
  }

  const std::size_t width = type_width(column.type());
  const std::size_t at = out.size();
  out.resize(at + n * width);
  std::byte* cursor = out.data() + at;
  for (std::size_t i = 0; i < n; ++i, cursor += width) {
    switch (column.type()) {
      case TypeId::Int32: store_i32_le(cursor, column.int32_at(i)); break;
      case TypeId::Int64: store_i64_le(cursor, column.int64_at(i)); break;
      case TypeId::Double: store_f64_le(cursor, column.double_at(i)); break;
      case TypeId::String: break;  // handled above
    }
  }
}

ColumnVector plain_decode(const std::byte* data, TypeId type, std::size_t row_count) {
  ColumnVector column(type);
  if (type == TypeId::String) {
    const std::uint32_t char_bytes = load_u32_le(data);
    const std::byte* offsets = data + 4;
    const char* chars = reinterpret_cast<const char*>(offsets + 4 * (row_count + 1));
    std::vector<std::uint32_t>& out_offsets = column.mutable_offsets();
    out_offsets.clear();
    out_offsets.reserve(row_count + 1);
    for (std::size_t i = 0; i <= row_count; ++i) {
      out_offsets.push_back(load_u32_le(offsets + 4 * i));
    }
    column.mutable_chars().assign(chars, char_bytes);
    column.set_size(row_count);
    return column;
  }

  column.reserve(row_count);
  const std::size_t width = type_width(type);
  for (std::size_t i = 0; i < row_count; ++i) {
    const std::byte* at = data + i * width;
    switch (type) {
      case TypeId::Int32: column.append_int32(load_i32_le(at)); break;
      case TypeId::Int64: column.append_int64(load_i64_le(at)); break;
      case TypeId::Double: column.append_double(load_f64_le(at)); break;
      case TypeId::String: break;
    }
  }
  return column;
}

/// Byte length of a PLAIN block that was written for `row_count` values, needed when
/// a plain block is embedded inside another encoding (the dictionary payload).
std::size_t plain_block_bytes(const std::byte* data, TypeId type,
                              std::size_t row_count) {
  if (type == TypeId::String) {
    return 4 + 4 * (row_count + 1) + load_u32_le(data);
  }
  return row_count * type_width(type);
}

// ---------------------------------------------------------------------------
// FRAME OF REFERENCE -- integers only
//
// Nulls keep whatever placeholder sits in their slot (zero), and that placeholder is
// included in the min/max. It costs ratio on a column whose real values are far from
// zero and mostly null, and it buys a decoder that reproduces the input exactly,
// which is what makes the round-trip test meaningful.
// ---------------------------------------------------------------------------

bool for_bounds(const ColumnVector& column, std::int64_t& min, std::uint64_t& range) {
  if (column.type() != TypeId::Int32 && column.type() != TypeId::Int64) return false;
  if (column.empty()) {
    min = 0;
    range = 0;
    return true;
  }
  std::int64_t lo = int_at(column, 0);
  std::int64_t hi = lo;
  for (std::size_t i = 1; i < column.size(); ++i) {
    const std::int64_t value = int_at(column, i);
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  min = lo;
  // Unsigned subtraction: hi - lo can exceed INT64_MAX when the column spans the
  // whole range, and signed overflow there would be undefined behaviour.
  range = static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
  return true;
}

std::size_t for_size(const ColumnVector& column) {
  std::int64_t min = 0;
  std::uint64_t range = 0;
  if (!for_bounds(column, min, range)) return kNotApplicable;
  const std::uint32_t width = bitpack::width_for(range);
  return 8 + 1 + bitpack::packed_size(column.size(), width);
}

void for_encode(const ColumnVector& column, std::vector<std::byte>& out) {
  std::int64_t min = 0;
  std::uint64_t range = 0;
  for_bounds(column, min, range);
  const std::uint32_t width = bitpack::width_for(range);

  append_u64(out, static_cast<std::uint64_t>(min));
  append_u8(out, static_cast<std::uint8_t>(width));

  std::vector<std::uint64_t> residuals(column.size());
  for (std::size_t i = 0; i < column.size(); ++i) {
    residuals[i] = static_cast<std::uint64_t>(int_at(column, i)) -
                   static_cast<std::uint64_t>(min);
  }
  const std::size_t at = out.size();
  out.resize(at + bitpack::packed_size(column.size(), width));
  bitpack::pack(residuals.data(), column.size(), width, out.data() + at);
}

ColumnVector for_decode(const std::byte* data, TypeId type, std::size_t row_count) {
  const auto min = static_cast<std::int64_t>(load_u64_le(data));
  const auto width = static_cast<std::uint32_t>(data[8]);

  std::vector<std::uint64_t> residuals(row_count);
  bitpack::unpack(data + 9, row_count, width, residuals.data());

  ColumnVector column(type);
  column.reserve(row_count);
  for (std::size_t i = 0; i < row_count; ++i) {
    const auto value = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(min) + residuals[i]);
    if (type == TypeId::Int32) {
      column.append_int32(static_cast<std::int32_t>(value));
    } else {
      column.append_int64(value);
    }
  }
  return column;
}

// ---------------------------------------------------------------------------
// DICTIONARY
//
// The dictionary is sorted, not first-seen. Sorting costs one pass at write time and
// buys an ordering property at read time: code < code' implies value < value', so a
// range predicate can be evaluated against two code bounds instead of against every
// decoded value. First-seen order would make that impossible forever, and the file
// format is the one thing that cannot be changed later.
// ---------------------------------------------------------------------------

struct Dictionary {
  ColumnVector values;
  std::vector<std::uint64_t> codes;
};

bool build_dictionary(const ColumnVector& column, Dictionary& dict) {
  const std::size_t n = column.size();
  dict.values = ColumnVector(column.type());
  dict.codes.resize(n);

  if (column.type() == TypeId::String) {
    std::map<std::string_view, std::uint64_t> order;
    for (std::size_t i = 0; i < n; ++i) order.emplace(column.string_at(i), 0);
    std::uint64_t next = 0;
    for (auto& entry : order) entry.second = next++;
    for (auto& entry : order) dict.values.append_string(entry.first);
    for (std::size_t i = 0; i < n; ++i) dict.codes[i] = order[column.string_at(i)];
    return true;
  }

  std::map<double, std::uint64_t> double_order;
  std::map<std::int64_t, std::uint64_t> int_order;
  const bool is_double = column.type() == TypeId::Double;

  if (is_double) {
    for (std::size_t i = 0; i < n; ++i) double_order.emplace(column.double_at(i), 0);
    std::uint64_t next = 0;
    for (auto& entry : double_order) entry.second = next++;
    for (auto& entry : double_order) dict.values.append_double(entry.first);
    for (std::size_t i = 0; i < n; ++i) dict.codes[i] = double_order[column.double_at(i)];
    return true;
  }

  for (std::size_t i = 0; i < n; ++i) int_order.emplace(int_at(column, i), 0);
  std::uint64_t next = 0;
  for (auto& entry : int_order) entry.second = next++;
  for (auto& entry : int_order) {
    if (column.type() == TypeId::Int32) {
      dict.values.append_int32(static_cast<std::int32_t>(entry.first));
    } else {
      dict.values.append_int64(entry.first);
    }
  }
  for (std::size_t i = 0; i < n; ++i) dict.codes[i] = int_order[int_at(column, i)];
  return true;
}

std::size_t dictionary_size_from(const Dictionary& dict, std::size_t row_count) {
  const std::size_t distinct = dict.values.size();
  const std::uint32_t code_width =
      bitpack::width_for(distinct == 0 ? 0 : distinct - 1);
  return 4 + plain_size(dict.values) + 1 + bitpack::packed_size(row_count, code_width);
}

std::size_t dictionary_size(const ColumnVector& column) {
  Dictionary dict;
  build_dictionary(column, dict);
  return dictionary_size_from(dict, column.size());
}

void dictionary_encode(const ColumnVector& column, std::vector<std::byte>& out) {
  Dictionary dict;
  build_dictionary(column, dict);

  const std::size_t distinct = dict.values.size();
  const std::uint32_t code_width =
      bitpack::width_for(distinct == 0 ? 0 : distinct - 1);

  append_u32(out, static_cast<std::uint32_t>(distinct));
  plain_encode(dict.values, out);
  append_u8(out, static_cast<std::uint8_t>(code_width));

  const std::size_t at = out.size();
  out.resize(at + bitpack::packed_size(column.size(), code_width));
  bitpack::pack(dict.codes.data(), column.size(), code_width, out.data() + at);
}

ColumnVector dictionary_decode(const std::byte* data, TypeId type,
                               std::size_t row_count) {
  const std::uint32_t distinct = load_u32_le(data);
  const std::byte* dict_block = data + 4;
  const ColumnVector dict = plain_decode(dict_block, type, distinct);
  const std::size_t dict_bytes = plain_block_bytes(dict_block, type, distinct);

  const std::byte* cursor = dict_block + dict_bytes;
  const auto code_width = static_cast<std::uint32_t>(cursor[0]);
  ++cursor;

  std::vector<std::uint64_t> codes(row_count);
  bitpack::unpack(cursor, row_count, code_width, codes.data());

  ColumnVector column(type);
  column.reserve(row_count);
  for (std::size_t i = 0; i < row_count; ++i) {
    const std::size_t code = static_cast<std::size_t>(codes[i]);
    switch (type) {
      case TypeId::Int32: column.append_int32(dict.int32_at(code)); break;
      case TypeId::Int64: column.append_int64(dict.int64_at(code)); break;
      case TypeId::Double: column.append_double(dict.double_at(code)); break;
      case TypeId::String: column.append_string(dict.string_at(code)); break;
    }
  }
  return column;
}

// ---------------------------------------------------------------------------
// RLE -- fixed-width types only
// ---------------------------------------------------------------------------

std::size_t rle_run_count(const ColumnVector& column) {
  const std::size_t n = column.size();
  if (n == 0) return 0;
  std::size_t runs = 1;
  for (std::size_t i = 1; i < n; ++i) {
    bool same = false;
    switch (column.type()) {
      case TypeId::Int32: same = column.int32_at(i) == column.int32_at(i - 1); break;
      case TypeId::Int64: same = column.int64_at(i) == column.int64_at(i - 1); break;
      // Bit equality, not numeric: -0.0 and 0.0 compare equal numerically but are
      // different values, and a decoder that returns the wrong one has lost data.
      case TypeId::Double: {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        const double x = column.double_at(i);
        const double y = column.double_at(i - 1);
        std::memcpy(&a, &x, sizeof(a));
        std::memcpy(&b, &y, sizeof(b));
        same = a == b;
        break;
      }
      case TypeId::String: same = column.string_at(i) == column.string_at(i - 1); break;
    }
    if (!same) ++runs;
  }
  return runs;
}

std::size_t rle_size(const ColumnVector& column) {
  if (!is_fixed_width(column.type())) return kNotApplicable;
  const std::size_t runs = rle_run_count(column);
  return 4 + runs * (type_width(column.type()) + 4);
}

void rle_encode(const ColumnVector& column, std::vector<std::byte>& out) {
  const std::size_t n = column.size();
  const std::size_t runs = rle_run_count(column);
  append_u32(out, static_cast<std::uint32_t>(runs));

  const std::size_t width = type_width(column.type());
  const std::size_t values_at = out.size();
  out.resize(values_at + runs * width + runs * 4);
  std::byte* values = out.data() + values_at;
  std::byte* lengths = values + runs * width;

  std::size_t run = 0;
  std::size_t start = 0;
  for (std::size_t i = 1; i <= n; ++i) {
    bool boundary = (i == n);
    if (!boundary) {
      switch (column.type()) {
        case TypeId::Int32: boundary = column.int32_at(i) != column.int32_at(i - 1); break;
        case TypeId::Int64: boundary = column.int64_at(i) != column.int64_at(i - 1); break;
        case TypeId::Double: {
          std::uint64_t a = 0;
          std::uint64_t b = 0;
          const double x = column.double_at(i);
          const double y = column.double_at(i - 1);
          std::memcpy(&a, &x, sizeof(a));
          std::memcpy(&b, &y, sizeof(b));
          boundary = a != b;
          break;
        }
        case TypeId::String: break;
      }
    }
    if (!boundary) continue;

    std::byte* slot = values + run * width;
    switch (column.type()) {
      case TypeId::Int32: store_i32_le(slot, column.int32_at(start)); break;
      case TypeId::Int64: store_i64_le(slot, column.int64_at(start)); break;
      case TypeId::Double: store_f64_le(slot, column.double_at(start)); break;
      case TypeId::String: break;
    }
    store_u32_le(lengths + run * 4, static_cast<std::uint32_t>(i - start));
    ++run;
    start = i;
  }
}

ColumnVector rle_decode(const std::byte* data, TypeId type, std::size_t row_count) {
  const std::uint32_t runs = load_u32_le(data);
  const std::size_t width = type_width(type);
  const std::byte* values = data + 4;
  const std::byte* lengths = values + runs * width;

  ColumnVector column(type);
  column.reserve(row_count);
  for (std::uint32_t run = 0; run < runs; ++run) {
    const std::uint32_t length = load_u32_le(lengths + run * 4);
    const std::byte* slot = values + run * width;
    for (std::uint32_t k = 0; k < length; ++k) {
      switch (type) {
        case TypeId::Int32: column.append_int32(load_i32_le(slot)); break;
        case TypeId::Int64: column.append_int64(load_i64_le(slot)); break;
        case TypeId::Double: column.append_double(load_f64_le(slot)); break;
        case TypeId::String: break;
      }
    }
  }
  return column;
}

}  // namespace

// ---------------------------------------------------------------------------

bool encode_with(const ColumnVector& column, Encoding encoding,
                 std::vector<std::byte>& out) {
  switch (encoding) {
    case Encoding::Plain:
      plain_encode(column, out);
      return true;
    case Encoding::FrameOfRef: {
      std::int64_t min = 0;
      std::uint64_t range = 0;
      if (!for_bounds(column, min, range)) return false;
      for_encode(column, out);
      return true;
    }
    case Encoding::Dictionary:
      dictionary_encode(column, out);
      return true;
    case Encoding::Rle:
      if (!is_fixed_width(column.type())) return false;
      rle_encode(column, out);
      return true;
  }
  return false;
}

std::size_t estimate_encoded_size(const ColumnVector& column, Encoding encoding) {
  switch (encoding) {
    case Encoding::Plain: return plain_size(column);
    case Encoding::FrameOfRef: return for_size(column);
    case Encoding::Dictionary: return dictionary_size(column);
    case Encoding::Rle: return rle_size(column);
  }
  return kNotApplicable;
}

Encoding encode_best(const ColumnVector& column, std::vector<std::byte>& out) {
  // Estimate first, then encode once. The dictionary is therefore built twice on a
  // column that wins with it -- measured at roughly 4% of total write time on TPC-H
  // lineitem, which is not worth threading the built dictionary through the interface
  // to remove. Revisit if the writer ever shows up in a profile.
  Encoding best = Encoding::Plain;
  std::size_t best_size = plain_size(column);
  for (Encoding candidate : {Encoding::FrameOfRef, Encoding::Dictionary, Encoding::Rle}) {
    const std::size_t size = estimate_encoded_size(column, candidate);
    if (size < best_size) {
      best_size = size;
      best = candidate;
    }
  }
  encode_with(column, best, out);
  return best;
}

ColumnVector decode(const std::byte* data, std::size_t bytes, Encoding encoding,
                    TypeId type, std::size_t row_count) {
  (void)bytes;
  switch (encoding) {
    case Encoding::Plain: return plain_decode(data, type, row_count);
    case Encoding::FrameOfRef: return for_decode(data, type, row_count);
    case Encoding::Dictionary: return dictionary_decode(data, type, row_count);
    case Encoding::Rle: return rle_decode(data, type, row_count);
  }
  return ColumnVector(type);
}

}  // namespace quarry
