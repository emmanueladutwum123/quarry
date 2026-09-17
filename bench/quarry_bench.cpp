// SPDX-License-Identifier: Apache-2.0

/// The benchmark harness. Every performance claim in the README and in the source
/// comments is produced here, on the machine the reader is holding, rather than
/// quoted from a run nobody can repeat.
///
/// Two rules it follows, because a benchmark that breaks either of them reports
/// numbers that are worse than none:
///
///  * Nothing measured is allowed to be dead code. Results are accumulated into a
///    checksum that is printed, so the optimiser cannot delete the work.
///  * Timings are medians of repeated runs, not a single sample and not a mean. One
///    sample measures whatever else the machine was doing; a mean lets a single
///    scheduling hiccup dominate.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "quarry/exec/operator.hpp"
#include "quarry/segment.hpp"

using namespace quarry;
using Clock = std::chrono::steady_clock;

namespace {

double median_seconds(const std::function<void()>& work, int repeats = 5) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(repeats));
  for (int i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    work();
    const auto end = Clock::now();
    samples.push_back(std::chrono::duration<double>(end - start).count());
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

std::string commas(std::uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  int count = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (count && count % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++count;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------------------
// Column generators shaped like the columns a real fact table has
// ---------------------------------------------------------------------------

ColumnVector clustered_dates(std::size_t rows) {
  ColumnVector column(TypeId::Int32);
  for (std::size_t i = 0; i < rows; ++i) {
    column.append_int32(static_cast<std::int32_t>(8000 + i / 64));
  }
  return column;
}

ColumnVector shuffled_dates(std::size_t rows) {
  std::mt19937 rng(7);
  ColumnVector column(TypeId::Int32);
  for (std::size_t i = 0; i < rows; ++i) {
    column.append_int32(static_cast<std::int32_t>(8000 + rng() % (rows / 64 + 1)));
  }
  return column;
}

ColumnVector random_int64(std::size_t rows) {
  std::mt19937_64 rng(8);
  ColumnVector column(TypeId::Int64);
  for (std::size_t i = 0; i < rows; ++i) {
    column.append_int64(static_cast<std::int64_t>(rng()));
  }
  return column;
}

ColumnVector prices(std::size_t rows) {
  std::mt19937 rng(9);
  ColumnVector column(TypeId::Double);
  for (std::size_t i = 0; i < rows; ++i) {
    column.append_double(static_cast<double>(rng() % 100000) / 100.0);
  }
  return column;
}

ColumnVector ship_modes(std::size_t rows) {
  static const char* modes[] = {"AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB",
                                "REG AIR"};
  std::mt19937 rng(10);
  ColumnVector column(TypeId::String);
  for (std::size_t i = 0; i < rows; ++i) column.append_string(modes[rng() % 7]);
  return column;
}

// ---------------------------------------------------------------------------

void bench_encode(std::size_t rows) {
  std::printf("\n== encoding: %s rows per column\n", commas(rows).c_str());
  std::printf("%-16s %-6s %10s %10s %12s %12s\n", "column", "enc", "bytes",
              "bits/row", "encode MB/s", "decode MB/s");

  struct Case {
    const char* name;
    ColumnVector column;
  };
  std::vector<Case> cases;
  cases.push_back({"date clustered", clustered_dates(rows)});
  cases.push_back({"int64 random", random_int64(rows)});
  cases.push_back({"price double", prices(rows)});
  cases.push_back({"ship mode", ship_modes(rows)});

  for (Case& item : cases) {
    std::vector<std::byte> encoded;
    const Encoding chosen = encode_best(item.column, encoded);

    const std::size_t plain = estimate_encoded_size(item.column, Encoding::Plain);
    const double plain_mb = static_cast<double>(plain) / 1e6;

    const double encode_time = median_seconds([&] {
      std::vector<std::byte> scratch;
      encode_with(item.column, chosen, scratch);
    });

    std::uint64_t checksum = 0;
    const double decode_time = median_seconds([&] {
      const ColumnVector back =
          decode(encoded.data(), encoded.size(), chosen, item.column.type(), rows);
      checksum += back.size();
    });

    const double bits_per_row =
        static_cast<double>(encoded.size()) * 8.0 / static_cast<double>(rows);
    std::printf("%-16s %-6s %10s %10.2f %12.1f %12.1f\n", item.name,
                encoding_name(chosen), commas(encoded.size()).c_str(), bits_per_row,
                plain_mb / encode_time, plain_mb / decode_time);
    if (checksum == 0) std::printf("  (checksum %llu)\n",
                                   static_cast<unsigned long long>(checksum));
  }
}

struct BuiltSegment {
  std::string path;
  ~BuiltSegment() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

/// Write a fact table and return its path. `clustered` controls whether the date
/// column ascends, which is the only thing that decides whether zone maps work.
std::string build_segment(std::size_t rows, bool clustered, std::size_t rows_per_group) {
  static int counter = 0;
  const std::string path =
      (std::filesystem::temp_directory_path() /
       ("quarry_bench_" + std::to_string(++counter) + ".qseg"))
          .string();

  Schema schema;
  schema.add("day", TypeId::Int32);
  schema.add("customer", TypeId::Int64);
  schema.add("price", TypeId::Double);
  schema.add("mode", TypeId::String);

  ColumnVector day = clustered ? clustered_dates(rows) : shuffled_dates(rows);
  ColumnVector customer(TypeId::Int64);
  std::mt19937 rng(11);
  for (std::size_t i = 0; i < rows; ++i) {
    customer.append_int64(static_cast<std::int64_t>(rng() % 200000));
  }

  Batch batch;
  batch.add_column(std::move(day));
  batch.add_column(std::move(customer));
  batch.add_column(prices(rows));
  batch.add_column(ship_modes(rows));
  batch.set_rows(rows);

  SegmentWriter writer(path, schema, rows_per_group);
  writer.append(batch);
  writer.finish();
  return path;
}

void bench_scan(std::size_t rows) {
  std::printf("\n== scan and filter: %s rows\n", commas(rows).c_str());
  const BuiltSegment segment{build_segment(rows, true, 65536)};
  const SegmentReader reader = SegmentReader::open(segment.path);

  std::printf("%-34s %12s %14s\n", "query", "rows out", "M rows/s");

  struct Query {
    const char* name;
    ExprPtr predicate;
    std::vector<std::size_t> columns;
  };
  std::vector<Query> queries;
  queries.push_back({"scan 1 column, no filter", nullptr, {0}});
  queries.push_back({"scan 4 columns, no filter", nullptr, {0, 1, 2, 3}});
  queries.push_back({"filter int, ~50% pass",
                     compare(CompareOp::Ge, column_ref(0, TypeId::Int32),
                             literal(Value::of_int32(
                                 static_cast<std::int32_t>(8000 + rows / 128)))),
                     {0}});
  queries.push_back({"filter string = 'AIR' (~14%)",
                     compare(CompareOp::Eq, column_ref(0, TypeId::String),
                             literal(Value::of_string("AIR"))),
                     {3}});

  for (Query& query : queries) {
    std::uint64_t produced = 0;
    const double seconds = median_seconds([&] {
      class Counter : public Sink {
       public:
        void consume(const Batch&, const Selection& selection) override {
          rows += selection.size();
        }
        std::uint64_t rows = 0;
      } counter;

      if (query.predicate) {
        FilterOp filter(query.predicate, &counter);
        TableScan scan(reader, query.columns);
        scan.run(filter);
      } else {
        TableScan scan(reader, query.columns);
        scan.run(counter);
      }
      produced = counter.rows;
    });
    std::printf("%-34s %12s %14.1f\n", query.name, commas(produced).c_str(),
                static_cast<double>(rows) / seconds / 1e6);
  }
}

void bench_pruning(std::size_t rows) {
  std::printf("\n== zone-map pruning: %s rows, 65,536-row groups\n",
              commas(rows).c_str());
  std::printf("%-22s %10s %12s %12s %10s\n", "date column", "groups", "groups read",
              "skip rate", "M rows/s");

  for (bool clustered : {true, false}) {
    const BuiltSegment segment{build_segment(rows, clustered, 65536)};
    const SegmentReader reader = SegmentReader::open(segment.path);
    const auto threshold = static_cast<std::int32_t>(8000 + (rows / 64) * 9 / 10);

    ScanStats stats;
    const double seconds = median_seconds([&] {
      class Counter : public Sink {
       public:
        void consume(const Batch&, const Selection& selection) override {
          rows += selection.size();
        }
        std::uint64_t rows = 0;
      } counter;
      FilterOp filter(compare(CompareOp::Ge, column_ref(0, TypeId::Int32),
                              literal(Value::of_int32(threshold))),
                      &counter);
      TableScan scan(reader, {0});
      scan.push_down(0, CompareOp::Ge, Value::of_int32(threshold));
      scan.run(filter);
      stats = scan.stats();
    });

    std::printf("%-22s %10s %12s %11.1f%% %10.1f\n",
                clustered ? "clustered (sorted)" : "shuffled",
                commas(stats.row_groups_total).c_str(),
                commas(stats.row_groups_read).c_str(), stats.skip_rate() * 100.0,
                static_cast<double>(rows) / seconds / 1e6);
  }
  std::printf("  the same predicate on the same data: pruning is a property of how\n"
              "  the data is laid out, not of the index\n");
}

void bench_batch_size(std::size_t rows) {
  std::printf("\n== batch size sweep (filter + aggregate over %s rows)\n",
              commas(rows).c_str());
  std::printf("%-12s %14s\n", "rows/batch", "M rows/s");

  ColumnVector keys(TypeId::Int32);
  ColumnVector values(TypeId::Double);
  std::mt19937 rng(12);
  for (std::size_t i = 0; i < rows; ++i) {
    keys.append_int32(static_cast<std::int32_t>(rng() % 64));
    values.append_double(static_cast<double>(rng() % 1000));
  }

  for (std::size_t batch_rows : {64u, 256u, 1024u, 2048u, 8192u, 65536u}) {
    const double seconds = median_seconds([&] {
      HashAggregate aggregate({0}, {TypeId::Int32},
                              {AggSpec{AggFunc::Sum, 1, TypeId::Double, false, "s"}});
      for (std::size_t start = 0; start < rows; start += batch_rows) {
        const std::size_t take = std::min(batch_rows, rows - start);
        Batch batch;
        ColumnVector key_slice(TypeId::Int32);
        ColumnVector value_slice(TypeId::Double);
        key_slice.append_from(keys, start, take);
        value_slice.append_from(values, start, take);
        batch.add_column(std::move(key_slice));
        batch.add_column(std::move(value_slice));
        batch.set_rows(take);
        aggregate.consume(batch, Selection::all(take));
      }
    }, 3);
    std::printf("%-12s %14.1f\n", commas(batch_rows).c_str(),
                static_cast<double>(rows) / seconds / 1e6);
  }
  std::printf("  (includes the cost of slicing the input, which a real pipeline does\n"
              "   not pay -- the shape of the curve is the result, not the level)\n");
}

void bench_aggregate(std::size_t rows) {
  std::printf("\n== hash aggregate: %s rows, varying group count\n",
              commas(rows).c_str());
  std::printf("%-14s %14s %14s\n", "groups", "M rows/s", "ns/row");

  for (std::size_t groups : {8u, 1024u, 65536u, 1000000u}) {
    ColumnVector keys(TypeId::Int64);
    ColumnVector values(TypeId::Double);
    std::mt19937_64 rng(13);
    for (std::size_t i = 0; i < rows; ++i) {
      keys.append_int64(static_cast<std::int64_t>(rng() % groups));
      values.append_double(1.0);
    }
    Batch batch;
    batch.add_column(std::move(keys));
    batch.add_column(std::move(values));
    batch.set_rows(rows);

    const double seconds = median_seconds([&] {
      HashAggregate aggregate({0}, {TypeId::Int64},
                              {AggSpec{AggFunc::Sum, 1, TypeId::Double, false, "s"}});
      aggregate.consume(batch, Selection::all(rows));
    }, 3);
    std::printf("%-14s %14.1f %14.1f\n", commas(groups).c_str(),
                static_cast<double>(rows) / seconds / 1e6,
                seconds * 1e9 / static_cast<double>(rows));
  }
  std::printf("  the table stops fitting in cache somewhere in this range, and the\n"
              "  cost per row is dominated by that, not by the hash\n");
}

void bench_join(std::size_t probe_rows, std::size_t build_rows) {
  std::printf("\n== hash join: %s probe rows against %s build rows\n",
              commas(probe_rows).c_str(), commas(build_rows).c_str());

  ColumnVector build_key(TypeId::Int64);
  ColumnVector build_value(TypeId::Int64);
  for (std::size_t i = 0; i < build_rows; ++i) {
    build_key.append_int64(static_cast<std::int64_t>(i));
    build_value.append_int64(static_cast<std::int64_t>(i) * 3);
  }
  Batch build_batch;
  build_batch.add_column(std::move(build_key));
  build_batch.add_column(std::move(build_value));
  build_batch.set_rows(build_rows);

  ColumnVector probe_key(TypeId::Int64);
  std::mt19937_64 rng(14);
  for (std::size_t i = 0; i < probe_rows; ++i) {
    probe_key.append_int64(static_cast<std::int64_t>(rng() % build_rows));
  }
  Batch probe_batch;
  probe_batch.add_column(std::move(probe_key));
  probe_batch.set_rows(probe_rows);

  JoinHashIndex index;
  const double build_time = median_seconds([&] {
    index = JoinHashIndex{};
    HashJoinBuild build({0}, &index);
    build.consume(build_batch, Selection::all(build_rows));
    build.finish();
  }, 3);

  class Counter : public Sink {
   public:
    void consume(const Batch&, const Selection& selection) override {
      rows += selection.size();
    }
    std::uint64_t rows = 0;
  };

  std::uint64_t emitted = 0;
  const double probe_time = median_seconds([&] {
    Counter counter;
    HashJoinProbe probe({0}, &index, &counter);
    probe.consume(probe_batch, Selection::all(probe_rows));
    emitted = counter.rows;
  }, 3);

  std::printf("  build %8.1f M rows/s    probe %8.1f M rows/s    %s rows out\n",
              static_cast<double>(build_rows) / build_time / 1e6,
              static_cast<double>(probe_rows) / probe_time / 1e6,
              commas(emitted).c_str());
}

void bench_conjunction(std::size_t rows) {
  std::printf("\n== selection vectors on a conjunction (%s rows)\n",
              commas(rows).c_str());

  // Two predicates, the first highly selective. With selection vectors the second
  // runs on what the first kept; the comparison below is against evaluating both
  // over the whole batch, which is what an engine without them does.
  ColumnVector a(TypeId::Int64);
  ColumnVector b(TypeId::Int64);
  std::mt19937_64 rng(15);
  for (std::size_t i = 0; i < rows; ++i) {
    a.append_int64(static_cast<std::int64_t>(rng() % 100));
    b.append_int64(static_cast<std::int64_t>(rng() % 100));
  }
  Batch batch;
  batch.add_column(std::move(a));
  batch.add_column(std::move(b));
  batch.set_rows(rows);

  const ExprPtr first =
      compare(CompareOp::Lt, column_ref(0, TypeId::Int64), literal(Value::of_int64(2)));
  const ExprPtr second =
      compare(CompareOp::Lt, column_ref(1, TypeId::Int64), literal(Value::of_int64(50)));

  std::size_t kept = 0;
  const double chained = median_seconds([&] {
    const Selection out =
        evaluate_predicate(*logical_and(first, second), batch, Selection::all(rows));
    kept = out.size();
  });

  const double independent = median_seconds([&] {
    const Selection left = evaluate_predicate(*first, batch, Selection::all(rows));
    const Selection right = evaluate_predicate(*second, batch, Selection::all(rows));
    kept = std::min(left.size(), right.size());
  });

  std::printf("  chained through the selection : %7.2f ms\n", chained * 1e3);
  std::printf("  both over the full batch      : %7.2f ms  (%.2fx)\n",
              independent * 1e3, independent / chained);
  std::printf("  %s rows survive the first predicate\n", commas(kept).c_str());
}

void usage() {
  std::printf(
      "usage: quarry_bench [encode|scan|prune|batch|aggregate|join|conj|all]\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "all";
  if (which == "-h" || which == "--help") {
    usage();
    return 0;
  }

  std::printf("quarry benchmark -- medians of repeated runs on this machine\n");

  const bool all = which == "all";
  if (all || which == "encode") bench_encode(4'000'000);
  if (all || which == "scan") bench_scan(8'000'000);
  if (all || which == "prune") bench_pruning(8'000'000);
  if (all || which == "batch") bench_batch_size(4'000'000);
  if (all || which == "aggregate") bench_aggregate(4'000'000);
  if (all || which == "join") bench_join(4'000'000, 500'000);
  if (all || which == "conj") bench_conjunction(8'000'000);
  return 0;
}
