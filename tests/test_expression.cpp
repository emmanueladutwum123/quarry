// SPDX-License-Identifier: Apache-2.0
#include "quarry/expression.hpp"
#include "test_harness.hpp"

#include <string>
#include <vector>

using namespace quarry;

namespace {

/// A three-column batch: value (int64, nullable), score (double), tag (string).
Batch make_batch() {
  ColumnVector value(TypeId::Int64);
  ColumnVector score(TypeId::Double);
  ColumnVector tag(TypeId::String);
  const char* tags[] = {"red", "green", "blue"};
  for (int i = 0; i < 100; ++i) {
    if (i % 10 == 7) {
      value.append_null();
    } else {
      value.append_int64(i);
    }
    score.append_double(static_cast<double>(i) / 2.0);
    tag.append_string(tags[i % 3]);
  }
  Batch batch;
  batch.add_column(std::move(value));
  batch.add_column(std::move(score));
  batch.add_column(std::move(tag));
  return batch;
}

std::vector<std::uint32_t> rows_of(const Selection& selection) {
  std::vector<std::uint32_t> out;
  for (std::size_t i = 0; i < selection.size(); ++i) out.push_back(selection[i]);
  return out;
}

}  // namespace

TEST(compare_column_against_literal) {
  const Batch batch = make_batch();
  const ExprPtr predicate =
      compare(CompareOp::Ge, column_ref(0, TypeId::Int64, "value"),
              literal(Value::of_int64(95)));

  const Selection kept =
      evaluate_predicate(*predicate, batch, Selection::all(batch.rows()));
  // 95, 96, 98, 99 -- row 97 is null and a comparison with null is not true.
  CHECK_EQ(kept.size(), std::size_t{4});
  const std::vector<std::uint32_t> rows = rows_of(kept);
  CHECK_EQ(rows[0], 95u);
  CHECK_EQ(rows[3], 99u);
}

TEST(nulls_never_satisfy_a_comparison) {
  const Batch batch = make_batch();
  for (CompareOp op : {CompareOp::Eq, CompareOp::Ne, CompareOp::Lt, CompareOp::Le,
                       CompareOp::Gt, CompareOp::Ge}) {
    const ExprPtr predicate = compare(op, column_ref(0, TypeId::Int64),
                                      literal(Value::of_int64(7)));
    const Selection kept =
        evaluate_predicate(*predicate, batch, Selection::all(batch.rows()));
    for (std::size_t i = 0; i < kept.size(); ++i) {
      CHECK(batch.column(0).is_valid(kept[i]));
    }
  }
}

TEST(conjunction_evaluates_the_second_term_only_on_survivors) {
  const Batch batch = make_batch();
  const ExprPtr predicate = logical_and(
      compare(CompareOp::Lt, column_ref(0, TypeId::Int64), literal(Value::of_int64(10))),
      compare(CompareOp::Eq, column_ref(2, TypeId::String),
              literal(Value::of_string("red"))));

  const Selection kept =
      evaluate_predicate(*predicate, batch, Selection::all(batch.rows()));
  // Rows 0..9 except the null at 7, and of those the ones where i % 3 == 0.
  const std::vector<std::uint32_t> rows = rows_of(kept);
  CHECK_EQ(rows.size(), std::size_t{4});  // 0, 3, 6, 9
  CHECK_EQ(rows[0], 0u);
  CHECK_EQ(rows[1], 3u);
  CHECK_EQ(rows[2], 6u);
  CHECK_EQ(rows[3], 9u);
}

TEST(disjunction_is_an_ordered_union_without_duplicates) {
  const Batch batch = make_batch();
  const ExprPtr predicate = logical_or(
      compare(CompareOp::Lt, column_ref(0, TypeId::Int64), literal(Value::of_int64(3))),
      compare(CompareOp::Le, column_ref(0, TypeId::Int64), literal(Value::of_int64(4))));

  const Selection kept =
      evaluate_predicate(*predicate, batch, Selection::all(batch.rows()));
  const std::vector<std::uint32_t> rows = rows_of(kept);
  CHECK_EQ(rows.size(), std::size_t{5});  // 0,1,2,3,4 -- each once
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK_EQ(rows[i], static_cast<std::uint32_t>(i));
  }
}

TEST(negation_returns_the_complement_of_the_input_not_of_the_batch) {
  const Batch batch = make_batch();
  // Restrict to the first ten rows, then negate inside that restriction.
  const ExprPtr first_ten =
      compare(CompareOp::Lt, column_ref(0, TypeId::Int64), literal(Value::of_int64(10)));
  const Selection base =
      evaluate_predicate(*first_ten, batch, Selection::all(batch.rows()));

  const ExprPtr negated = logical_not(
      compare(CompareOp::Lt, column_ref(0, TypeId::Int64), literal(Value::of_int64(5))));
  const Selection kept = evaluate_predicate(*negated, batch, base);

  const std::vector<std::uint32_t> rows = rows_of(kept);
  CHECK_EQ(rows.size(), std::size_t{4});  // 5, 6, 8, 9 -- 7 is null, excluded by base
  CHECK_EQ(rows[0], 5u);
  CHECK_EQ(rows[3], 9u);
}

TEST(arithmetic_promotes_int_to_double) {
  const Batch batch = make_batch();
  const ExprPtr expr = arithmetic(ArithOp::Mul, column_ref(1, TypeId::Double),
                                  literal(Value::of_int64(2)));
  CHECK_EQ(result_type(*expr), TypeId::Double);

  const ColumnVector out = evaluate_value(*expr, batch, Selection::all(batch.rows()));
  CHECK_EQ(out.size(), std::size_t{100});
  CHECK_EQ(out.double_at(4), 4.0);
  CHECK_EQ(out.double_at(99), 99.0);
}

TEST(arithmetic_propagates_nulls) {
  const Batch batch = make_batch();
  const ExprPtr expr = arithmetic(ArithOp::Add, column_ref(0, TypeId::Int64),
                                  literal(Value::of_int64(1)));
  const ColumnVector out = evaluate_value(*expr, batch, Selection::all(batch.rows()));
  CHECK(!out.is_valid(7));
  CHECK(out.is_valid(8));
  CHECK_EQ(out.int64_at(8), std::int64_t{9});
}

TEST(a_selection_carries_positions_through_a_value_expression) {
  const Batch batch = make_batch();
  const ExprPtr predicate =
      compare(CompareOp::Ge, column_ref(0, TypeId::Int64), literal(Value::of_int64(96)));
  const Selection kept =
      evaluate_predicate(*predicate, batch, Selection::all(batch.rows()));

  // The produced column is as long as the selection, not as long as the batch.
  const ColumnVector out =
      evaluate_value(*column_ref(1, TypeId::Double), batch, kept);
  CHECK_EQ(out.size(), kept.size());
  CHECK_EQ(out.double_at(0), 48.0);  // row 96
}

TEST(expressions_print_as_readable_plans) {
  const ExprPtr predicate = logical_and(
      compare(CompareOp::Ge, column_ref(0, TypeId::Int64, "day"),
              literal(Value::of_int64(100))),
      compare(CompareOp::Eq, column_ref(2, TypeId::String, "mode"),
              literal(Value::of_string("AIR"))));
  CHECK_EQ(predicate->to_string(), std::string("((day >= 100) AND (mode = 'AIR'))"));
}
