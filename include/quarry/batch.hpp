// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A `Batch` is the unit of work everywhere above the storage layer: a few thousand
/// rows of every column an operator needs, travelling together.
///
/// Batch size is the one tuning knob in the execution engine and it is a real
/// trade-off, not a constant someone picked. Too small and the per-batch overhead --
/// a virtual call, a loop setup, a bounds check -- is amortised over too few rows.
/// Too large and the working set stops fitting in L2, so every operator in the
/// pipeline streams its input back in from memory instead of finding it warm. 2048
/// rows of eight 8-byte columns is 128KB, which is the neighbourhood where both costs
/// are small; `bench/` measures the curve rather than trusting the reasoning.

#include <cstddef>
#include <utility>
#include <vector>

#include "quarry/column.hpp"
#include "quarry/schema.hpp"

namespace quarry {

constexpr std::size_t kDefaultBatchRows = 2048;

class Batch {
 public:
  Batch() = default;
  explicit Batch(std::vector<ColumnVector> columns) : columns_(std::move(columns)) {
    if (!columns_.empty()) rows_ = columns_[0].size();
  }

  std::size_t rows() const { return rows_; }
  std::size_t width() const { return columns_.size(); }
  bool empty() const { return rows_ == 0; }

  const ColumnVector& column(std::size_t index) const { return columns_[index]; }
  ColumnVector& mutable_column(std::size_t index) { return columns_[index]; }
  const std::vector<ColumnVector>& columns() const { return columns_; }

  void add_column(ColumnVector column) {
    if (columns_.empty()) rows_ = column.size();
    columns_.push_back(std::move(column));
  }

  void set_rows(std::size_t rows) { rows_ = rows; }
  void clear() {
    columns_.clear();
    rows_ = 0;
  }

 private:
  std::vector<ColumnVector> columns_;
  std::size_t rows_ = 0;
};

}  // namespace quarry
