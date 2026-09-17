// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Zone maps: the min, max and null count of every column chunk, kept in the footer.
///
/// This is the cheapest optimisation in an analytics engine and usually the largest.
/// A predicate like `ship_date >= '1995-01-01'` can be answered for an entire row
/// group -- 65,536 rows -- by comparing two values from the footer, and a row group
/// that cannot contain a match is never read from disk, never decoded, and never
/// filtered. The work saved is not a constant factor; it is the difference between
/// scanning the column and not scanning it.
///
/// What makes it work is *clustering*, not sorting: a date column in insertion order
/// is already nearly sorted in practice, so its zone maps are narrow and disjoint. A
/// randomly ordered column has a zone map spanning the whole domain in every row
/// group, which prunes nothing -- the structure is the same, the benefit is zero, and
/// `bench/` reports the actual skip rate rather than assuming the happy case.

#include <cstdint>

#include "quarry/types.hpp"

namespace quarry {

enum class CompareOp : std::uint8_t { Eq, Ne, Lt, Le, Gt, Ge };

struct ZoneMap {
  Value min;
  Value max;
  std::uint64_t null_count = 0;
  std::uint64_t row_count = 0;
  bool has_values = false;  ///< false when every row in the chunk is null

  /// Can any row in this chunk satisfy `column <op> literal`?
  ///
  /// Conservative by construction: a false positive costs one wasted chunk read, a
  /// false negative silently drops rows from the answer. Every case that is not
  /// provably empty therefore returns true.
  bool can_contain(CompareOp op, const Value& literal) const {
    if (!has_values) return false;
    if (literal.type != min.type) return true;  // planner's problem, not ours

    switch (op) {
      case CompareOp::Eq:
        return min.compare(literal) <= 0 && max.compare(literal) >= 0;
      case CompareOp::Ne:
        // Only prunable when the chunk holds exactly one distinct value and that
        // value is the one being excluded.
        return !(min.compare(max) == 0 && min.compare(literal) == 0);
      case CompareOp::Lt: return min.compare(literal) < 0;
      case CompareOp::Le: return min.compare(literal) <= 0;
      case CompareOp::Gt: return max.compare(literal) > 0;
      case CompareOp::Ge: return max.compare(literal) >= 0;
    }
    return true;
  }
};

constexpr const char* compare_op_name(CompareOp op) {
  switch (op) {
    case CompareOp::Eq: return "=";
    case CompareOp::Ne: return "<>";
    case CompareOp::Lt: return "<";
    case CompareOp::Le: return "<=";
    case CompareOp::Gt: return ">";
    case CompareOp::Ge: return ">=";
  }
  return "?";
}

}  // namespace quarry
