// SPDX-License-Identifier: Apache-2.0
#pragma once

/// `ColumnVector` -- a run of values of one type, which is the only shape data takes
/// once it is inside the engine.
///
/// Every operator consumes and produces these. Nothing in the execution layer ever
/// sees a row: a row is an index into several column vectors, materialised only at
/// the very top when results are printed. That is the whole reason a columnar engine
/// is fast, and it is a discipline rather than a data structure -- the moment one
/// operator builds a `struct Row`, the property is gone.
///
/// Strings are offset-encoded (`offsets[i] .. offsets[i+1]` into one character
/// buffer) rather than a `vector<string>`: 8 million strings is 8 million heap
/// allocations and 8 million pointer chases, and the scan is bandwidth-bound already.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "quarry/bitmap.hpp"
#include "quarry/types.hpp"

namespace quarry {

class ColumnVector {
 public:
  ColumnVector() = default;
  explicit ColumnVector(TypeId type) : type_(type) {
    if (type_ == TypeId::String) offsets_.push_back(0);
  }

  TypeId type() const { return type_; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  /// True when no value in the vector is null, which lets operators take a branch-free
  /// path. An empty validity bitmap means "all valid" -- the common case costs nothing.
  bool all_valid() const { return validity_.empty(); }
  bool is_valid(std::size_t index) const {
    return validity_.empty() || validity_.get(index);
  }
  const Bitmap& validity() const { return validity_; }

  // ---- typed access -------------------------------------------------------
  // The asserts are the contract: calling int32_at on a DOUBLE column is a bug in the
  // planner, not a runtime condition to branch on, so it is checked in debug builds
  // and costs nothing in release.

  std::int32_t int32_at(std::size_t index) const {
    assert(type_ == TypeId::Int32);
    return int32_data()[index];
  }
  std::int64_t int64_at(std::size_t index) const {
    assert(type_ == TypeId::Int64);
    return int64_data()[index];
  }
  double double_at(std::size_t index) const {
    assert(type_ == TypeId::Double);
    return double_data()[index];
  }
  std::string_view string_at(std::size_t index) const {
    assert(type_ == TypeId::String);
    const std::uint32_t begin = offsets_[index];
    const std::uint32_t end = offsets_[index + 1];
    return std::string_view(chars_.data() + begin, end - begin);
  }

  const std::int32_t* int32_data() const {
    return reinterpret_cast<const std::int32_t*>(values_.data());
  }
  const std::int64_t* int64_data() const {
    return reinterpret_cast<const std::int64_t*>(values_.data());
  }
  const double* double_data() const {
    return reinterpret_cast<const double*>(values_.data());
  }
  std::int32_t* mutable_int32_data() {
    return reinterpret_cast<std::int32_t*>(values_.data());
  }
  std::int64_t* mutable_int64_data() {
    return reinterpret_cast<std::int64_t*>(values_.data());
  }
  double* mutable_double_data() { return reinterpret_cast<double*>(values_.data()); }

  const std::vector<std::uint32_t>& offsets() const { return offsets_; }
  const std::string& chars() const { return chars_; }

  // ---- building -----------------------------------------------------------

  void append_int32(std::int32_t value) {
    append_bytes(&value, sizeof(value));
    grow_validity(true);
  }
  void append_int64(std::int64_t value) {
    append_bytes(&value, sizeof(value));
    grow_validity(true);
  }
  void append_double(double value) {
    append_bytes(&value, sizeof(value));
    grow_validity(true);
  }
  void append_string(std::string_view value) {
    chars_.append(value);
    offsets_.push_back(static_cast<std::uint32_t>(chars_.size()));
    ++size_;
    grow_validity(true);
  }

  /// Append a null. The value slot still advances -- a column vector is positional,
  /// so row i of every column in a batch must be at index i in each of them.
  void append_null() {
    materialise_validity();
    if (type_ == TypeId::String) {
      offsets_.push_back(static_cast<std::uint32_t>(chars_.size()));
      ++size_;
    } else {
      const std::size_t width = type_width(type_);
      values_.resize(values_.size() + width, std::byte{0});
      ++size_;
    }
    validity_.push_back(false);
  }

  void append_value(const Value& value) {
    switch (type_) {
      case TypeId::Int32: append_int32(value.i32); break;
      case TypeId::Int64: append_int64(value.i64); break;
      case TypeId::Double: append_double(value.f64); break;
      case TypeId::String: append_string(value.str); break;
    }
  }

  Value value_at(std::size_t index) const {
    switch (type_) {
      case TypeId::Int32: return Value::of_int32(int32_at(index));
      case TypeId::Int64: return Value::of_int64(int64_at(index));
      case TypeId::Double: return Value::of_double(double_at(index));
      case TypeId::String: return Value::of_string(std::string(string_at(index)));
    }
    return {};
  }

  /// Reserve for `rows` fixed-width values. Not valid for strings, whose character
  /// budget is not known from the row count.
  void reserve(std::size_t rows) {
    if (is_fixed_width(type_)) values_.reserve(rows * type_width(type_));
  }

  void set_validity(Bitmap validity) { validity_ = std::move(validity); }

  /// Append `count` rows of `source` starting at `begin`. The writer uses this to
  /// accumulate batches into a row group without materialising rows in between.
  void append_from(const ColumnVector& source, std::size_t begin, std::size_t count) {
    for (std::size_t i = begin; i < begin + count; ++i) {
      if (!source.is_valid(i)) {
        append_null();
        continue;
      }
      switch (type_) {
        case TypeId::Int32: append_int32(source.int32_at(i)); break;
        case TypeId::Int64: append_int64(source.int64_at(i)); break;
        case TypeId::Double: append_double(source.double_at(i)); break;
        case TypeId::String: append_string(source.string_at(i)); break;
      }
    }
  }

  /// Drop the validity bitmap when every row turned out to be present. Worth doing
  /// once at the end of a chunk: it moves every later null check onto the fast path.
  void compact_validity() {
    if (!validity_.empty() && validity_.all_set()) validity_.clear();
  }

  void clear() {
    values_.clear();
    chars_.clear();
    offsets_.clear();
    if (type_ == TypeId::String) offsets_.push_back(0);
    validity_.clear();
    size_ = 0;
  }

  /// Raw fixed-width bytes, for the encoder and for zero-copy writes.
  const std::vector<std::byte>& raw() const { return values_; }
  std::vector<std::byte>& mutable_raw() { return values_; }
  std::vector<std::uint32_t>& mutable_offsets() { return offsets_; }
  std::string& mutable_chars() { return chars_; }
  void set_size(std::size_t size) { size_ = size; }

 private:
  void append_bytes(const void* data, std::size_t bytes) {
    const auto* source = static_cast<const std::byte*>(data);
    values_.insert(values_.end(), source, source + bytes);
    ++size_;
  }

  void grow_validity(bool valid) {
    if (!validity_.empty()) validity_.push_back(valid);
  }

  /// A vector starts with no bitmap and stays that way until the first null arrives;
  /// at that point every earlier row has to be retroactively marked valid.
  void materialise_validity() {
    if (validity_.empty() && size_ > 0) validity_ = Bitmap(size_, true);
  }

  TypeId type_ = TypeId::Int32;
  std::size_t size_ = 0;
  std::vector<std::byte> values_;      ///< fixed-width payload
  std::vector<std::uint32_t> offsets_; ///< string offsets, size() + 1 entries
  std::string chars_;                  ///< string character pool
  Bitmap validity_;
};

}  // namespace quarry
