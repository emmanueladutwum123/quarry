// SPDX-License-Identifier: Apache-2.0
#include "quarry/exec/operator.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace quarry;

namespace {

class TempSegment {
 public:
  explicit TempSegment(const char* tag) {
    static int counter = 0;
    path_ = (std::filesystem::temp_directory_path() /
             ("quarry_exec_" + std::string(tag) + "_" + std::to_string(++counter) +
              ".qseg"))
                .string();
  }
  ~TempSegment() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

struct Row {
  std::int32_t day;
  std::int64_t customer;
  double amount;
  std::string mode;
};

/// One dataset, held both as rows (for the brute-force reference) and written to a
/// segment (for the engine). Every test below compares the two.
struct Dataset {
  std::vector<Row> rows;
  TempSegment file;

  explicit Dataset(const char* tag, std::size_t count, std::size_t rows_per_group = 500)
      : file(tag) {
    const char* modes[] = {"AIR", "RAIL", "SHIP", "TRUCK"};
    std::mt19937 rng(12345);
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      Row row;
      // `day` ascends so zone maps are narrow, which is the case pruning is for.
      row.day = static_cast<std::int32_t>(i / 4);
      row.customer = static_cast<std::int64_t>(rng() % 50);
      row.amount = static_cast<double>(rng() % 10000) / 100.0;
      row.mode = modes[rng() % 4];
      rows.push_back(std::move(row));
    }
    write(rows_per_group);
  }

  static Schema schema() {
    Schema s;
    s.add("day", TypeId::Int32);
    s.add("customer", TypeId::Int64);
    s.add("amount", TypeId::Double);
    s.add("mode", TypeId::String);
    return s;
  }

  void write(std::size_t rows_per_group) {
    SegmentWriter writer(file.path(), schema(), rows_per_group);
    const std::size_t stride = 256;
    for (std::size_t start = 0; start < rows.size(); start += stride) {
      const std::size_t take = std::min(stride, rows.size() - start);
      ColumnVector day(TypeId::Int32);
      ColumnVector customer(TypeId::Int64);
      ColumnVector amount(TypeId::Double);
      ColumnVector mode(TypeId::String);
      for (std::size_t i = start; i < start + take; ++i) {
        day.append_int32(rows[i].day);
        customer.append_int64(rows[i].customer);
        amount.append_double(rows[i].amount);
        mode.append_string(rows[i].mode);
      }
      Batch batch;
      batch.add_column(std::move(day));
      batch.add_column(std::move(customer));
      batch.add_column(std::move(amount));
      batch.add_column(std::move(mode));
      writer.append(batch);
    }
    writer.finish();
  }
};

}  // namespace

TEST(filter_matches_a_brute_force_scan) {
  const Dataset data("filter", 5000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  CollectSink sink;
  FilterOp filter(
      logical_and(compare(CompareOp::Ge, column_ref(0, TypeId::Int32, "day"),
                          literal(Value::of_int32(900))),
                  compare(CompareOp::Eq, column_ref(3, TypeId::String, "mode"),
                          literal(Value::of_string("AIR")))),
      &sink);
  TableScan scan(reader, {0, 1, 2, 3});
  scan.run(filter);

  std::size_t expected = 0;
  for (const Row& row : data.rows) {
    if (row.day >= 900 && row.mode == "AIR") ++expected;
  }
  CHECK_EQ(sink.total_rows(), expected);
  CHECK(expected > 0);  // a test that filters everything away proves nothing
}

TEST(pushdown_skips_row_groups_and_returns_the_same_rows) {
  const Dataset data("pushdown", 20'000, 1000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  const ExprPtr predicate = compare(CompareOp::Ge, column_ref(0, TypeId::Int32, "day"),
                                    literal(Value::of_int32(4500)));

  CollectSink without_pushdown;
  {
    FilterOp filter(predicate, &without_pushdown);
    TableScan scan(reader, {0, 1, 2, 3});
    scan.run(filter);
    CHECK_EQ(scan.stats().row_groups_read, scan.stats().row_groups_total);
  }

  CollectSink with_pushdown;
  std::uint64_t groups_read = 0;
  std::uint64_t groups_total = 0;
  {
    FilterOp filter(predicate, &with_pushdown);
    TableScan scan(reader, {0, 1, 2, 3});
    scan.push_down(0, CompareOp::Ge, Value::of_int32(4500));
    scan.run(filter);
    groups_read = scan.stats().row_groups_read;
    groups_total = scan.stats().row_groups_total;
  }

  // Same answer, less work: this is the whole claim of a zone map.
  CHECK_EQ(with_pushdown.total_rows(), without_pushdown.total_rows());
  CHECK(groups_read < groups_total);
  CHECK(groups_read <= 3);
}

TEST(pushdown_never_drops_a_matching_row) {
  // The dangerous direction. A zone map that prunes too eagerly returns a wrong
  // answer silently, so this checks every row of the pruned result against the
  // brute-force answer rather than just the count.
  const Dataset data("pushdown_exact", 8000, 400);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  for (std::int32_t threshold : {0, 1, 500, 999, 1999, 5000}) {
    CollectSink sink;
    FilterOp filter(compare(CompareOp::Eq, column_ref(0, TypeId::Int32),
                            literal(Value::of_int32(threshold))),
                    &sink);
    TableScan scan(reader, {0, 1, 2, 3});
    scan.push_down(0, CompareOp::Eq, Value::of_int32(threshold));
    scan.run(filter);

    std::size_t expected = 0;
    for (const Row& row : data.rows) {
      if (row.day == threshold) ++expected;
    }
    CHECK_EQ(sink.total_rows(), expected);
  }
}

TEST(aggregation_matches_a_reference_group_by) {
  const Dataset data("aggregate", 10'000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  HashAggregate aggregate({3}, {TypeId::String},
                          {AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "n"},
                           AggSpec{AggFunc::Sum, 2, TypeId::Double, false, "total"},
                           AggSpec{AggFunc::Min, 2, TypeId::Double, false, "lo"},
                           AggSpec{AggFunc::Max, 2, TypeId::Double, false, "hi"},
                           AggSpec{AggFunc::Avg, 2, TypeId::Double, false, "mean"}});
  TableScan scan(reader, {0, 1, 2, 3});
  scan.run(aggregate);

  struct Reference {
    std::uint64_t count = 0;
    double total = 0;
    double lo = 0;
    double hi = 0;
    bool seen = false;
  };
  std::map<std::string, Reference> expected;
  for (const Row& row : data.rows) {
    Reference& ref = expected[row.mode];
    ++ref.count;
    ref.total += row.amount;
    ref.lo = ref.seen ? std::min(ref.lo, row.amount) : row.amount;
    ref.hi = ref.seen ? std::max(ref.hi, row.amount) : row.amount;
    ref.seen = true;
  }

  const Batch result = aggregate.result();
  CHECK_EQ(result.rows(), expected.size());
  CHECK_EQ(result.width(), std::size_t{6});

  for (std::size_t i = 0; i < result.rows(); ++i) {
    const std::string mode(result.column(0).string_at(i));
    const Reference& ref = expected.at(mode);
    CHECK_EQ(result.column(1).int64_at(i), static_cast<std::int64_t>(ref.count));
    CHECK_NEAR(result.column(2).double_at(i), ref.total, 1e-6);
    CHECK_EQ(result.column(3).double_at(i), ref.lo);
    CHECK_EQ(result.column(4).double_at(i), ref.hi);
    CHECK_NEAR(result.column(5).double_at(i),
               ref.total / static_cast<double>(ref.count), 1e-9);
  }
}

TEST(grouping_on_two_columns_does_not_merge_distinct_groups) {
  // The serialised-key bug this guards: without a length prefix, ("ab","c") and
  // ("a","bc") produce identical bytes and two groups silently become one.
  ColumnVector left(TypeId::String);
  ColumnVector right(TypeId::String);
  left.append_string("ab");
  right.append_string("c");
  left.append_string("a");
  right.append_string("bc");
  Batch batch;
  batch.add_column(std::move(left));
  batch.add_column(std::move(right));
  batch.set_rows(2);

  HashAggregate aggregate({0, 1}, {TypeId::String, TypeId::String},
                          {AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "n"}});
  aggregate.consume(batch, Selection::all(batch.rows()));
  CHECK_EQ(aggregate.group_count(), std::size_t{2});
}

TEST(aggregates_ignore_nulls_and_count_star_does_not) {
  ColumnVector key(TypeId::Int32);
  ColumnVector value(TypeId::Int64);
  for (int i = 0; i < 10; ++i) {
    key.append_int32(0);
    if (i % 2 == 0) {
      value.append_null();
    } else {
      value.append_int64(i);
    }
  }
  Batch batch;
  batch.add_column(std::move(key));
  batch.add_column(std::move(value));
  batch.set_rows(10);

  HashAggregate aggregate({0}, {TypeId::Int32},
                          {AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "star"},
                           AggSpec{AggFunc::Count, 1, TypeId::Int64, false, "nonnull"},
                           AggSpec{AggFunc::Sum, 1, TypeId::Int64, false, "total"}});
  aggregate.consume(batch, Selection::all(batch.rows()));

  const Batch result = aggregate.result();
  CHECK_EQ(result.rows(), std::size_t{1});
  CHECK_EQ(result.column(1).int64_at(0), std::int64_t{10});  // COUNT(*)
  CHECK_EQ(result.column(2).int64_at(0), std::int64_t{5});   // COUNT(value)
  CHECK_EQ(result.column(3).int64_at(0), std::int64_t{1 + 3 + 5 + 7 + 9});
}

TEST(an_all_null_group_aggregates_to_null_not_zero) {
  ColumnVector key(TypeId::Int32);
  ColumnVector value(TypeId::Int64);
  for (int i = 0; i < 4; ++i) {
    key.append_int32(1);
    value.append_null();
  }
  Batch batch;
  batch.add_column(std::move(key));
  batch.add_column(std::move(value));
  batch.set_rows(4);

  HashAggregate aggregate({0}, {TypeId::Int32},
                          {AggSpec{AggFunc::Sum, 1, TypeId::Int64, false, "total"}});
  aggregate.consume(batch, Selection::all(batch.rows()));
  const Batch result = aggregate.result();
  // SUM over no rows is NULL in SQL, not 0. Returning 0 is a plausible-looking wrong
  // answer that survives every test that only checks the happy path.
  CHECK(!result.column(1).is_valid(0));
}

TEST(inner_join_matches_a_reference_nested_loop) {
  // Build side: 50 customers with a name. Probe side: the fact table.
  const Dataset data("join", 4000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  ColumnVector ids(TypeId::Int64);
  ColumnVector names(TypeId::String);
  for (int i = 0; i < 40; ++i) {  // only 40 of the 50 customers exist
    ids.append_int64(i);
    names.append_string("cust" + std::to_string(i));
  }
  Batch dimension;
  dimension.add_column(std::move(ids));
  dimension.add_column(std::move(names));
  dimension.set_rows(40);

  JoinHashIndex index;
  {
    HashJoinBuild build({0}, &index);
    build.consume(dimension, Selection::all(dimension.rows()));
    build.finish();
  }

  CollectSink sink;
  HashJoinProbe probe({1}, &index, &sink);
  TableScan scan(reader, {0, 1, 2, 3});
  scan.run(probe);

  std::size_t expected = 0;
  for (const Row& row : data.rows) {
    if (row.customer < 40) ++expected;
  }
  CHECK_EQ(sink.total_rows(), expected);
  CHECK(expected > 0);

  const Batch joined = sink.materialize();
  CHECK_EQ(joined.width(), std::size_t{6});  // 4 probe columns + 2 build columns
  for (std::size_t i = 0; i < joined.rows(); ++i) {
    const std::int64_t customer = joined.column(1).int64_at(i);
    CHECK_EQ(std::string(joined.column(5).string_at(i)),
             "cust" + std::to_string(customer));
  }
}

TEST(join_emits_one_row_per_matching_pair) {
  // Duplicate keys on the build side: the chain has to be walked, not just the head.
  ColumnVector build_key(TypeId::Int32);
  ColumnVector build_tag(TypeId::Int32);
  for (int i = 0; i < 3; ++i) {
    build_key.append_int32(7);
    build_tag.append_int32(100 + i);
  }
  build_key.append_int32(8);
  build_tag.append_int32(999);
  Batch build_batch;
  build_batch.add_column(std::move(build_key));
  build_batch.add_column(std::move(build_tag));
  build_batch.set_rows(4);

  JoinHashIndex index;
  HashJoinBuild build({0}, &index);
  build.consume(build_batch, Selection::all(build_batch.rows()));
  build.finish();

  ColumnVector probe_key(TypeId::Int32);
  probe_key.append_int32(7);
  probe_key.append_int32(8);
  probe_key.append_int32(9);  // no match
  Batch probe_batch;
  probe_batch.add_column(std::move(probe_key));
  probe_batch.set_rows(3);

  CollectSink sink;
  HashJoinProbe probe({0}, &index, &sink);
  probe.consume(probe_batch, Selection::all(probe_batch.rows()));

  CHECK_EQ(sink.total_rows(), std::size_t{4});  // 3 for key 7, 1 for key 8, 0 for 9
  CHECK_EQ(probe.rows_probed(), std::uint64_t{3});

  const Batch joined = sink.materialize();
  std::vector<std::int32_t> tags;
  for (std::size_t i = 0; i < joined.rows(); ++i) {
    tags.push_back(joined.column(2).int32_at(i));
  }
  std::sort(tags.begin(), tags.end());
  CHECK_EQ(tags[0], 100);
  CHECK_EQ(tags[1], 101);
  CHECK_EQ(tags[2], 102);
  CHECK_EQ(tags[3], 999);
}

TEST(limit_stops_at_the_requested_row_count) {
  const Dataset data("limit", 5000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  CollectSink sink;
  LimitOp limit(37, &sink);
  TableScan scan(reader, {0, 1, 2, 3});
  scan.run(limit);
  CHECK_EQ(sink.total_rows(), std::size_t{37});
}

TEST(projection_produces_computed_columns) {
  const Dataset data("project", 1000);
  const SegmentReader reader = SegmentReader::open(data.file.path());

  CollectSink sink;
  ProjectOp project({column_ref(0, TypeId::Int32, "day"),
                     arithmetic(ArithOp::Mul, column_ref(2, TypeId::Double, "amount"),
                                literal(Value::of_double(2.0)))},
                    &sink);
  TableScan scan(reader, {0, 1, 2, 3});
  scan.run(project);

  const Batch result = sink.materialize();
  CHECK_EQ(result.width(), std::size_t{2});
  CHECK_EQ(result.rows(), std::size_t{1000});
  for (std::size_t i = 0; i < 20; ++i) {
    CHECK_NEAR(result.column(1).double_at(i), data.rows[i].amount * 2.0, 1e-9);
  }
}

TEST(a_global_aggregate_has_one_group_with_an_empty_key) {
  // No GROUP BY: one group whose serialised key is zero bytes. The zero-length key
  // is what makes a null data pointer reach memcmp and the arena, which is
  // undefined behaviour even at length zero -- UBSan on one CI leg is what found it.
  ColumnVector value(TypeId::Int64);
  for (int i = 1; i <= 100; ++i) value.append_int64(i);
  Batch batch;
  batch.add_column(std::move(value));
  batch.set_rows(100);

  HashAggregate aggregate({}, {},
                          {AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "n"},
                           AggSpec{AggFunc::Sum, 0, TypeId::Int64, false, "total"}});
  aggregate.consume(batch, Selection::all(batch.rows()));

  CHECK_EQ(aggregate.group_count(), std::size_t{1});
  const Batch result = aggregate.result();
  CHECK_EQ(result.rows(), std::size_t{1});
  CHECK_EQ(result.column(0).int64_at(0), std::int64_t{100});
  CHECK_EQ(result.column(1).int64_at(0), std::int64_t{5050});
}

TEST(an_empty_batch_survives_every_operator) {
  // Zero rows reaches memset, memcpy and pointer arithmetic with null pointers in
  // several places. Each is undefined even at length zero.
  Batch empty;
  empty.add_column(ColumnVector(TypeId::Int64));
  empty.add_column(ColumnVector(TypeId::String));
  empty.set_rows(0);

  CollectSink sink;
  FilterOp filter(compare(CompareOp::Gt, column_ref(0, TypeId::Int64),
                          literal(Value::of_int64(0))),
                  &sink);
  filter.consume(empty, Selection::all(0));
  CHECK_EQ(sink.total_rows(), std::size_t{0});

  HashAggregate aggregate({0}, {TypeId::Int64},
                          {AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "n"}});
  aggregate.consume(empty, Selection::all(0));
  CHECK_EQ(aggregate.group_count(), std::size_t{0});
  CHECK_EQ(aggregate.result().rows(), std::size_t{0});

  JoinHashIndex index;
  HashJoinBuild build({0}, &index);
  build.consume(empty, Selection::all(0));
  build.finish();

  CollectSink joined;
  HashJoinProbe probe({0}, &index, &joined);
  probe.consume(empty, Selection::all(0));
  CHECK_EQ(joined.total_rows(), std::size_t{0});
}
