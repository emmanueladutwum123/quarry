// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A selection vector: the row indices of a batch that are still alive.
///
/// The alternative is to materialise a new batch after every filter -- copy the
/// surviving values of every column into fresh buffers. That is simple and it is
/// what a first implementation always does, and it costs a full copy of the data at
/// each step of the pipeline.
///
/// A selection vector instead says "rows 3, 17 and 94 survived" and leaves the
/// columns untouched. The next predicate evaluates on those three rows and no
/// others, which is where the real win is: in `a > 5 AND b = 'x'`, the second
/// comparison runs on the rows that passed the first, not on the batch. On a
/// selective conjunction that is the difference between two full passes and one.
///
/// The cost is indirection -- `values[selection[i]]` instead of `values[i]` -- which
/// defeats some auto-vectorisation. So the "all rows selected" case is represented
/// explicitly and takes a dense path, because the first predicate in every pipeline
/// hits it and a scan with no filter hits it forever.

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace quarry {

class Selection {
 public:
  Selection() = default;

  /// Everything in a batch of `rows` rows, with no indirection.
  static Selection all(std::size_t rows) {
    Selection selection;
    selection.all_ = true;
    selection.count_ = rows;
    return selection;
  }

  static Selection of(std::vector<std::uint32_t> indices) {
    Selection selection;
    selection.all_ = false;
    selection.count_ = indices.size();
    selection.indices_ = std::move(indices);
    return selection;
  }

  bool is_all() const { return all_; }
  std::size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }

  /// The i'th selected row index. Callers on the hot path branch once on `is_all()`
  /// and then use a dense loop rather than calling this per row.
  std::uint32_t operator[](std::size_t i) const {
    return all_ ? static_cast<std::uint32_t>(i) : indices_[i];
  }

  /// Materialise the indices, so a caller that needs an array does not have to
  /// special-case the dense form.
  const std::vector<std::uint32_t>& indices() {
    if (all_ && indices_.size() != count_) {
      indices_.resize(count_);
      std::iota(indices_.begin(), indices_.end(), std::uint32_t{0});
    }
    return indices_;
  }

 private:
  bool all_ = true;
  std::size_t count_ = 0;
  std::vector<std::uint32_t> indices_;
};

}  // namespace quarry
