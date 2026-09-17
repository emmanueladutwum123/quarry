// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Expressions, and the vectorized interpreter that evaluates them.
///
/// The interpreter is a tree walk, but it walks the tree once per *batch* rather than
/// once per row: the per-node dispatch cost is paid 2048 times less often than in a
/// row-at-a-time engine, which is most of the gap between the two designs and does
/// not require a compiler. Generating machine code per query closes the rest of the
/// gap and costs a JIT; that trade is noted in docs/design.md and not taken here.

#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/selection.hpp"
#include "quarry/types.hpp"
#include "quarry/zone_map.hpp"

namespace quarry {

enum class ExprKind : std::uint8_t {
  Column,
  Literal,
  Compare,
  And,
  Or,
  Not,
  Arithmetic,
};

enum class ArithOp : std::uint8_t { Add, Sub, Mul, Div };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Literal;
  TypeId type = TypeId::Int32;

  std::size_t column_index = 0;   ///< Column
  std::string column_name;        ///< Column, for plan printing
  Value literal;                  ///< Literal
  CompareOp compare_op = CompareOp::Eq;
  ArithOp arith_op = ArithOp::Add;
  std::vector<ExprPtr> children;

  std::string to_string() const;
};

ExprPtr column_ref(std::size_t index, TypeId type, std::string name = {});
ExprPtr literal(Value value);
ExprPtr compare(CompareOp op, ExprPtr left, ExprPtr right);
ExprPtr logical_and(ExprPtr left, ExprPtr right);
ExprPtr logical_or(ExprPtr left, ExprPtr right);
ExprPtr logical_not(ExprPtr child);
ExprPtr arithmetic(ArithOp op, ExprPtr left, ExprPtr right);

/// Evaluate a predicate over the rows in `input`, returning those that pass.
///
/// Conjunctions are evaluated left to right with the output of one term feeding the
/// next, so a term only ever sees rows that survived everything to its left.
Selection evaluate_predicate(const Expr& expr, const Batch& batch, const Selection& input);

/// Evaluate a value expression over the selected rows, producing one value per
/// selected row (so the result is `selection.size()` long, not `batch.rows()`).
ColumnVector evaluate_value(const Expr& expr, const Batch& batch,
                            const Selection& selection);

/// The type a value expression produces, checked once at plan time so the evaluator
/// can assume it rather than branching on it per batch.
TypeId result_type(const Expr& expr);

}  // namespace quarry
