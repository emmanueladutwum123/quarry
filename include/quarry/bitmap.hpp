// SPDX-License-Identifier: Apache-2.0
#pragma once

/// A validity bitmap: one bit per row, set when the value is present.
///
/// Stored as a bitmap rather than a `vector<bool>` of the engine's own making, or a
/// byte per row, because the null check sits inside every scan, filter and hash
/// probe. A byte per row costs 8x the memory bandwidth for information that is
/// usually uniform -- and the uniform case is what makes it cheap: a chunk with no
/// nulls carries no bitmap at all, and every operator checks that once per batch
/// instead of once per row.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quarry {

class Bitmap {
 public:
  Bitmap() = default;

  explicit Bitmap(std::size_t bits, bool value = true)
      : bits_(bits), words_((bits + 63) / 64, value ? ~std::uint64_t{0} : 0) {
    if (value) trim_tail();
  }

  std::size_t size() const { return bits_; }
  bool empty() const { return bits_ == 0; }

  bool get(std::size_t index) const {
    return (words_[index >> 6] >> (index & 63)) & 1;
  }

  void set(std::size_t index, bool value) {
    const std::uint64_t mask = std::uint64_t{1} << (index & 63);
    if (value) {
      words_[index >> 6] |= mask;
    } else {
      words_[index >> 6] &= ~mask;
    }
  }

  void push_back(bool value) {
    if ((bits_ & 63) == 0) words_.push_back(0);
    ++bits_;
    set(bits_ - 1, value);
  }

  void resize(std::size_t bits, bool value = true) {
    const std::size_t old = bits_;
    words_.resize((bits + 63) / 64, value ? ~std::uint64_t{0} : 0);
    bits_ = bits;
    for (std::size_t i = old; i < bits && (i & 63) != 0; ++i) set(i, value);
    trim_tail();
  }

  /// Population count. Used to decide whether a bitmap is worth storing at all.
  std::size_t count_set() const {
    std::size_t total = 0;
    for (std::uint64_t word : words_) {
      total += static_cast<std::size_t>(__builtin_popcountll(word));
    }
    return total;
  }

  bool all_set() const { return count_set() == bits_; }

  const std::uint64_t* words() const { return words_.data(); }
  std::size_t word_count() const { return words_.size(); }

  void clear() {
    bits_ = 0;
    words_.clear();
  }

 private:
  /// Bits past `bits_` in the final word must stay zero or `count_set` overcounts.
  void trim_tail() {
    const std::size_t used = bits_ & 63;
    if (used != 0 && !words_.empty()) {
      words_.back() &= (std::uint64_t{1} << used) - 1;
    }
  }

  std::size_t bits_ = 0;
  std::vector<std::uint64_t> words_;
};

}  // namespace quarry
