// SPDX-License-Identifier: Apache-2.0
#include "quarry/expression.hpp"

#include <algorithm>
#include <cmath>

#include "quarry/schema.hpp"

namespace quarry {
namespace {

template <typename T>
bool apply_compare(CompareOp op, const T& left, const T& right) {
  switch (op) {
    case CompareOp::Eq: return left == right;
    case CompareOp::Ne: return !(left == right);
    case CompareOp::Lt: return left < right;
    case CompareOp::Le: return !(right < left);
    case CompareOp::Gt: return right < left;
    case CompareOp::Ge: return !(left < right);
  }
  return false;
}

/// Compare a column against a constant over a selection.
///
/// This is the shape the planner tries hardest to produce, and it is the only
/// comparison with a dense fast path: when nothing has been filtered yet, the loop
/// is over `values[i]` with no indirection and no null check, which is what lets the
/// compiler vectorise it.
template <typename T, typename Reader>
void compare_column_constant(CompareOp op, const ColumnVector& column, Reader read,
                             const T& constant, const Selection& input,
                             std::vector<std::uint32_t>& out) {
  const std::size_t n = input.size();
  if (input.is_all() && column.all_valid()) {
    for (std::size_t i = 0; i < n; ++i) {
      if (apply_compare(op, read(column, i), constant)) {
        out.push_back(static_cast<std::uint32_t>(i));
      }
    }
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t row = input[i];
    // SQL three-valued logic: a comparison with NULL is unknown, and a WHERE clause
    // keeps only rows that are true. Unknown is therefore dropped -- which is not
    // the same as false, and matters as soon as a NOT sits above this.
    if (!column.is_valid(row)) continue;
    if (apply_compare(op, read(column, row), constant)) out.push_back(row);
  }
}

template <typename T, typename Reader>
void compare_column_column(CompareOp op, const ColumnVector& left,
                           const ColumnVector& right, Reader read,
                           const Selection& input, std::vector<std::uint32_t>& out) {
  const std::size_t n = input.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t row = input[i];
    if (!left.is_valid(row) || !right.is_valid(row)) continue;
    if (apply_compare(op, read(left, row), read(right, row))) out.push_back(row);
  }
}

std::int32_t read_i32(const ColumnVector& c, std::size_t i) { return c.int32_at(i); }
std::int64_t read_i64(const ColumnVector& c, std::size_t i) { return c.int64_at(i); }
double read_f64(const ColumnVector& c, std::size_t i) { return c.double_at(i); }
std::string_view read_str(const ColumnVector& c, std::size_t i) {
  return c.string_at(i);
}

/// A comparison whose operands are both simple enough to read straight out of the
/// batch avoids materialising an intermediate column. Anything more complex (an
/// arithmetic subtree) is evaluated into a temporary first.
bool is_direct_column(const Expr& expr) { return expr.kind == ExprKind::Column; }
bool is_literal(const Expr& expr) { return expr.kind == ExprKind::Literal; }

Selection compare_selection(const Expr& expr, const Batch& batch,
                            const Selection& input) {
  const Expr& left = *expr.children[0];
  const Expr& right = *expr.children[1];
  std::vector<std::uint32_t> out;
  out.reserve(input.size());

  if (is_direct_column(left) && is_literal(right)) {
    const ColumnVector& column = batch.column(left.column_index);
    switch (column.type()) {
      case TypeId::Int32:
        compare_column_constant<std::int32_t>(expr.compare_op, column, read_i32,
                                              right.literal.i32, input, out);
        break;
      case TypeId::Int64:
        compare_column_constant<std::int64_t>(expr.compare_op, column, read_i64,
                                              right.literal.i64, input, out);
        break;
      case TypeId::Double:
        compare_column_constant<double>(expr.compare_op, column, read_f64,
                                        right.literal.f64, input, out);
        break;
      case TypeId::String: {
        const std::string_view constant(right.literal.str);
        compare_column_constant<std::string_view>(expr.compare_op, column, read_str,
                                                  constant, input, out);
        break;
      }
    }
    return Selection::of(std::move(out));
  }

  if (is_direct_column(left) && is_direct_column(right)) {
    const ColumnVector& lhs = batch.column(left.column_index);
    const ColumnVector& rhs = batch.column(right.column_index);
    if (lhs.type() != rhs.type()) throw QuarryError("compare: operand types differ");
    switch (lhs.type()) {
      case TypeId::Int32:
        compare_column_column<std::int32_t>(expr.compare_op, lhs, rhs, read_i32, input,
                                            out);
        break;
      case TypeId::Int64:
        compare_column_column<std::int64_t>(expr.compare_op, lhs, rhs, read_i64, input,
                                            out);
        break;
      case TypeId::Double:
        compare_column_column<double>(expr.compare_op, lhs, rhs, read_f64, input, out);
        break;
      case TypeId::String:
        compare_column_column<std::string_view>(expr.compare_op, lhs, rhs, read_str,
                                                input, out);
        break;
    }
    return Selection::of(std::move(out));
  }

  // General case: materialise both sides for the selected rows, then compare
  // position by position.
  const ColumnVector lhs = evaluate_value(left, batch, input);
  const ColumnVector rhs = evaluate_value(right, batch, input);
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (!lhs.is_valid(i) || !rhs.is_valid(i)) continue;
    const Value a = lhs.value_at(i);
    const Value b = rhs.value_at(i);
    const int order = a.compare(b);
    bool keep = false;
    switch (expr.compare_op) {
      case CompareOp::Eq: keep = order == 0; break;
      case CompareOp::Ne: keep = order != 0; break;
      case CompareOp::Lt: keep = order < 0; break;
      case CompareOp::Le: keep = order <= 0; break;
      case CompareOp::Gt: keep = order > 0; break;
      case CompareOp::Ge: keep = order >= 0; break;
    }
    if (keep) out.push_back(input[i]);
  }
  return Selection::of(std::move(out));
}

template <typename T>
T apply_arith(ArithOp op, T left, T right) {
  switch (op) {
    case ArithOp::Add: return static_cast<T>(left + right);
    case ArithOp::Sub: return static_cast<T>(left - right);
    case ArithOp::Mul: return static_cast<T>(left * right);
    case ArithOp::Div: return right == T{0} ? T{0} : static_cast<T>(left / right);
  }
  return T{};
}

}  // namespace

// ---------------------------------------------------------------------------

ExprPtr column_ref(std::size_t index, TypeId type, std::string name) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Column;
  expr->type = type;
  expr->column_index = index;
  expr->column_name = std::move(name);
  return expr;
}

ExprPtr literal(Value value) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Literal;
  expr->type = value.type;
  expr->literal = std::move(value);
  return expr;
}

ExprPtr compare(CompareOp op, ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Compare;
  expr->compare_op = op;
  expr->children = {std::move(left), std::move(right)};
  return expr;
}

ExprPtr logical_and(ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::And;
  expr->children = {std::move(left), std::move(right)};
  return expr;
}

ExprPtr logical_or(ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Or;
  expr->children = {std::move(left), std::move(right)};
  return expr;
}

ExprPtr logical_not(ExprPtr child) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Not;
  expr->children = {std::move(child)};
  return expr;
}

ExprPtr arithmetic(ArithOp op, ExprPtr left, ExprPtr right) {
  auto expr = std::make_shared<Expr>();
  expr->kind = ExprKind::Arithmetic;
  expr->arith_op = op;
  expr->children = {std::move(left), std::move(right)};
  expr->type = result_type(*expr);
  return expr;
}

TypeId result_type(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Column:
    case ExprKind::Literal:
      return expr.type;
    case ExprKind::Arithmetic: {
      const TypeId left = result_type(*expr.children[0]);
      const TypeId right = result_type(*expr.children[1]);
      // Promote to the wider of the two. Mixed int/double arithmetic yields double,
      // which is the only promotion this type system needs.
      if (left == TypeId::Double || right == TypeId::Double) return TypeId::Double;
      if (left == TypeId::Int64 || right == TypeId::Int64) return TypeId::Int64;
      return TypeId::Int32;
    }
    default:
      return TypeId::Int32;  // predicates are consumed as selections, not values
  }
}

Selection evaluate_predicate(const Expr& expr, const Batch& batch,
                             const Selection& input) {
  switch (expr.kind) {
    case ExprKind::Compare:
      return compare_selection(expr, batch, input);

    case ExprKind::And: {
      // The whole point of selection vectors: the right-hand term is evaluated only
      // on the rows the left-hand term kept.
      const Selection left = evaluate_predicate(*expr.children[0], batch, input);
      if (left.empty()) return left;
      return evaluate_predicate(*expr.children[1], batch, left);
    }

    case ExprKind::Or: {
      const Selection left = evaluate_predicate(*expr.children[0], batch, input);
      const Selection right = evaluate_predicate(*expr.children[1], batch, input);
      // Both sides are ascending because every evaluator preserves input order, so
      // the union is a merge rather than a sort.
      std::vector<std::uint32_t> merged;
      merged.reserve(left.size() + right.size());
      std::size_t i = 0;
      std::size_t j = 0;
      while (i < left.size() && j < right.size()) {
        if (left[i] < right[j]) {
          merged.push_back(left[i++]);
        } else if (right[j] < left[i]) {
          merged.push_back(right[j++]);
        } else {
          merged.push_back(left[i++]);
          ++j;
        }
      }
      while (i < left.size()) merged.push_back(left[i++]);
      while (j < right.size()) merged.push_back(right[j++]);
      return Selection::of(std::move(merged));
    }

    case ExprKind::Not: {
      const Selection kept = evaluate_predicate(*expr.children[0], batch, input);
      std::vector<std::uint32_t> inverted;
      inverted.reserve(input.size() - kept.size());
      std::size_t k = 0;
      for (std::size_t i = 0; i < input.size(); ++i) {
        const std::uint32_t row = input[i];
        if (k < kept.size() && kept[k] == row) {
          ++k;
          continue;
        }
        inverted.push_back(row);
      }
      return Selection::of(std::move(inverted));
    }

    case ExprKind::Literal: {
      // A constant predicate: either everything or nothing. The planner folds these
      // away, but a hand-built plan can still contain one.
      const bool truth = expr.literal.type == TypeId::Int32 ? expr.literal.i32 != 0
                                                            : expr.literal.i64 != 0;
      if (truth) return input;
      return Selection::of({});
    }

    default:
      throw QuarryError("expression is not a predicate");
  }
}

ColumnVector evaluate_value(const Expr& expr, const Batch& batch,
                            const Selection& selection) {
  switch (expr.kind) {
    case ExprKind::Column: {
      const ColumnVector& source = batch.column(expr.column_index);
      ColumnVector out(source.type());
      out.reserve(selection.size());
      out.append_from_selection(source, selection);
      return out;
    }

    case ExprKind::Literal: {
      ColumnVector out(expr.literal.type);
      out.reserve(selection.size());
      for (std::size_t i = 0; i < selection.size(); ++i) out.append_value(expr.literal);
      return out;
    }

    case ExprKind::Arithmetic: {
      const ColumnVector left = evaluate_value(*expr.children[0], batch, selection);
      const ColumnVector right = evaluate_value(*expr.children[1], batch, selection);
      const TypeId type = result_type(expr);
      ColumnVector out(type);
      out.reserve(selection.size());

      for (std::size_t i = 0; i < selection.size(); ++i) {
        if (!left.is_valid(i) || !right.is_valid(i)) {
          out.append_null();
          continue;
        }
        switch (type) {
          case TypeId::Double: {
            const double a = left.type() == TypeId::Double ? left.double_at(i)
                             : left.type() == TypeId::Int64
                                 ? static_cast<double>(left.int64_at(i))
                                 : static_cast<double>(left.int32_at(i));
            const double b = right.type() == TypeId::Double ? right.double_at(i)
                             : right.type() == TypeId::Int64
                                 ? static_cast<double>(right.int64_at(i))
                                 : static_cast<double>(right.int32_at(i));
            out.append_double(apply_arith(expr.arith_op, a, b));
            break;
          }
          case TypeId::Int64: {
            const std::int64_t a = left.type() == TypeId::Int64
                                       ? left.int64_at(i)
                                       : static_cast<std::int64_t>(left.int32_at(i));
            const std::int64_t b = right.type() == TypeId::Int64
                                       ? right.int64_at(i)
                                       : static_cast<std::int64_t>(right.int32_at(i));
            out.append_int64(apply_arith(expr.arith_op, a, b));
            break;
          }
          case TypeId::Int32:
            out.append_int32(
                apply_arith(expr.arith_op, left.int32_at(i), right.int32_at(i)));
            break;
          case TypeId::String:
            throw QuarryError("arithmetic on a string column");
        }
      }
      return out;
    }

    default:
      throw QuarryError("expression is not a value expression");
  }
}

std::string Expr::to_string() const {
  switch (kind) {
    case ExprKind::Column:
      return column_name.empty() ? "#" + std::to_string(column_index) : column_name;
    case ExprKind::Literal:
      return literal.type == TypeId::String ? "'" + literal.str + "'"
                                            : literal.to_string();
    case ExprKind::Compare:
      return "(" + children[0]->to_string() + " " + compare_op_name(compare_op) + " " +
             children[1]->to_string() + ")";
    case ExprKind::And:
      return "(" + children[0]->to_string() + " AND " + children[1]->to_string() + ")";
    case ExprKind::Or:
      return "(" + children[0]->to_string() + " OR " + children[1]->to_string() + ")";
    case ExprKind::Not:
      return "(NOT " + children[0]->to_string() + ")";
    case ExprKind::Arithmetic: {
      const char* symbol = arith_op == ArithOp::Add   ? " + "
                           : arith_op == ArithOp::Sub ? " - "
                           : arith_op == ArithOp::Mul ? " * "
                                                      : " / ";
      return "(" + children[0]->to_string() + symbol + children[1]->to_string() + ")";
    }
  }
  return "?";
}

}  // namespace quarry
