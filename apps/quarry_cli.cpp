// SPDX-License-Identifier: Apache-2.0

/// `quarry` -- inspect a segment, or run a scan against one.
///
/// Deliberately not a SQL shell yet: a parser that accepts three query shapes is a
/// worse interface than a flag, because it implies the other shapes work. The
/// planner and SQL front end are the next milestone; until then this exposes the
/// engine's actual surface, including the scan counters, which is the part worth
/// looking at.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "quarry/exec/operator.hpp"
#include "quarry/segment.hpp"

using namespace quarry;

namespace {

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

int cmd_gen(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: quarry gen <file> <rows> [rows-per-group]\n");
    return 2;
  }
  const std::string path = argv[0];
  const auto rows = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  const std::size_t rows_per_group =
      argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 65536;

  Schema schema;
  schema.add("day", TypeId::Int32);
  schema.add("customer", TypeId::Int64);
  schema.add("price", TypeId::Double);
  schema.add("mode", TypeId::String);

  static const char* modes[] = {"AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB",
                                "REG AIR"};
  std::mt19937_64 rng(42);

  SegmentWriter writer(path, schema, rows_per_group);
  const std::size_t stride = 65536;
  for (std::size_t start = 0; start < rows; start += stride) {
    const std::size_t take = std::min(stride, rows - start);
    ColumnVector day(TypeId::Int32);
    ColumnVector customer(TypeId::Int64);
    ColumnVector price(TypeId::Double);
    ColumnVector mode(TypeId::String);
    for (std::size_t i = start; i < start + take; ++i) {
      day.append_int32(static_cast<std::int32_t>(8000 + i / 64));
      customer.append_int64(static_cast<std::int64_t>(rng() % 200000));
      price.append_double(static_cast<double>(rng() % 100000) / 100.0);
      mode.append_string(modes[rng() % 7]);
    }
    Batch batch;
    batch.add_column(std::move(day));
    batch.add_column(std::move(customer));
    batch.add_column(std::move(price));
    batch.add_column(std::move(mode));
    writer.append(batch);
  }
  writer.finish();
  std::printf("wrote %s rows to %s\n", commas(rows).c_str(), path.c_str());
  return 0;
}

int cmd_info(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr, "usage: quarry info <file>\n");
    return 2;
  }
  const SegmentReader reader = SegmentReader::open(argv[0]);
  const Schema& schema = reader.schema();

  std::printf("rows       : %s\n", commas(reader.total_rows()).c_str());
  std::printf("row groups : %s\n", commas(reader.row_group_count()).c_str());
  std::printf("columns    : %zu\n\n", schema.size());

  std::printf("%-14s %-8s %-6s %14s %9s %10s  %s\n", "column", "type", "enc", "bytes",
              "bits/row", "nulls", "range");
  for (std::size_t c = 0; c < schema.size(); ++c) {
    std::uint64_t bytes = 0;
    std::uint64_t nulls = 0;
    std::uint64_t rows = 0;
    Value low;
    Value high;
    bool seen = false;
    // Encodings can differ per row group -- the chooser runs per chunk. Report the
    // one the first group picked and note when the others disagree, rather than
    // implying a column has a single encoding.
    Encoding first_encoding = Encoding::Plain;
    bool mixed = false;

    for (std::size_t g = 0; g < reader.row_group_count(); ++g) {
      const ChunkMeta& chunk = reader.row_group(g).columns[c];
      bytes += chunk.bytes + chunk.validity_bytes;
      nulls += chunk.zone_map.null_count;
      rows += chunk.zone_map.row_count;
      if (g == 0) {
        first_encoding = chunk.encoding;
      } else if (chunk.encoding != first_encoding) {
        mixed = true;
      }
      if (chunk.zone_map.has_values) {
        if (!seen) {
          low = chunk.zone_map.min;
          high = chunk.zone_map.max;
          seen = true;
        } else {
          if (chunk.zone_map.min.compare(low) < 0) low = chunk.zone_map.min;
          if (chunk.zone_map.max.compare(high) > 0) high = chunk.zone_map.max;
        }
      }
    }

    const double bits = rows == 0 ? 0.0
                                  : static_cast<double>(bytes) * 8.0 /
                                        static_cast<double>(rows);
    std::string encoding_label = encoding_name(first_encoding);
    if (mixed) encoding_label += "*";
    std::printf("%-14s %-8s %-6s %14s %9.2f %10s  %s\n", schema[c].name.c_str(),
                std::string(type_name(schema[c].type)).c_str(), encoding_label.c_str(),
                commas(bytes).c_str(), bits, commas(nulls).c_str(),
                seen ? (low.to_string() + " .. " + high.to_string()).c_str() : "(all null)");
  }
  if (reader.row_group_count() > 1) {
    std::printf("\n* encoding differs between row groups; the chooser runs per chunk\n");
  }
  return 0;
}

CompareOp parse_op(const std::string& text) {
  if (text == "=" || text == "==") return CompareOp::Eq;
  if (text == "!=" || text == "<>") return CompareOp::Ne;
  if (text == "<") return CompareOp::Lt;
  if (text == "<=") return CompareOp::Le;
  if (text == ">") return CompareOp::Gt;
  if (text == ">=") return CompareOp::Ge;
  throw QuarryError("unknown operator: " + text);
}

Value parse_literal(TypeId type, const std::string& text) {
  switch (type) {
    case TypeId::Int32:
      return Value::of_int32(static_cast<std::int32_t>(std::strtol(text.c_str(), nullptr, 10)));
    case TypeId::Int64:
      return Value::of_int64(std::strtoll(text.c_str(), nullptr, 10));
    case TypeId::Double: return Value::of_double(std::strtod(text.c_str(), nullptr));
    case TypeId::String: return Value::of_string(text);
  }
  return {};
}

int cmd_scan(int argc, char** argv) {
  if (argc < 1) {
    std::fprintf(stderr,
                 "usage: quarry scan <file> [--where COL OP VALUE] [--no-pushdown]\n"
                 "                         [--group-by COL --sum COL] [--limit N]\n");
    return 2;
  }
  const SegmentReader reader = SegmentReader::open(argv[0]);
  const Schema& schema = reader.schema();

  std::string where_column;
  std::string where_op;
  std::string where_value;
  std::string group_column;
  std::string sum_column;
  bool pushdown = true;
  std::size_t limit = 10;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--where" && i + 3 < argc) {
      where_column = argv[++i];
      where_op = argv[++i];
      where_value = argv[++i];
    } else if (flag == "--no-pushdown") {
      pushdown = false;
    } else if (flag == "--group-by" && i + 1 < argc) {
      group_column = argv[++i];
    } else if (flag == "--sum" && i + 1 < argc) {
      sum_column = argv[++i];
    } else if (flag == "--limit" && i + 1 < argc) {
      limit = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", flag.c_str());
      return 2;
    }
  }

  std::vector<std::size_t> columns;
  for (std::size_t c = 0; c < schema.size(); ++c) columns.push_back(c);

  ExprPtr predicate;
  std::size_t where_index = 0;
  CompareOp op = CompareOp::Eq;
  Value literal_value;
  if (!where_column.empty()) {
    where_index = schema.index_of_or_throw(where_column);
    op = parse_op(where_op);
    literal_value = parse_literal(schema[where_index].type, where_value);
    predicate = compare(op, column_ref(where_index, schema[where_index].type,
                                       where_column),
                        literal(literal_value));
  }

  TableScan scan(reader, columns);
  if (predicate && pushdown) scan.push_down(where_index, op, literal_value);

  if (!group_column.empty()) {
    const std::size_t key = schema.index_of_or_throw(group_column);
    std::vector<AggSpec> aggregates{
        AggSpec{AggFunc::Count, 0, TypeId::Int64, true, "count"}};
    if (!sum_column.empty()) {
      const std::size_t value = schema.index_of_or_throw(sum_column);
      aggregates.push_back(
          AggSpec{AggFunc::Sum, value, schema[value].type, false, "sum"});
    }
    HashAggregate aggregate({key}, {schema[key].type}, aggregates);
    if (predicate) {
      FilterOp filter(predicate, &aggregate);
      scan.run(filter);
    } else {
      scan.run(aggregate);
    }

    const Batch result = aggregate.result();
    std::printf("%-20s %14s %18s\n", group_column.c_str(), "count",
                sum_column.empty() ? "" : "sum");
    for (std::size_t i = 0; i < result.rows() && i < limit; ++i) {
      std::printf("%-20s %14s", result.column(0).value_at(i).to_string().c_str(),
                  commas(static_cast<std::uint64_t>(result.column(1).int64_at(i))).c_str());
      if (result.width() > 2) {
        std::printf(" %18s", result.column(2).value_at(i).to_string().c_str());
      }
      std::printf("\n");
    }
    std::printf("\n%s groups\n", commas(result.rows()).c_str());
  } else {
    CollectSink sink;
    LimitOp limiter(limit, &sink);
    if (predicate) {
      FilterOp filter(predicate, &limiter);
      scan.run(filter);
    } else {
      scan.run(limiter);
    }
    const Batch rows = sink.materialize();
    for (std::size_t c = 0; c < schema.size(); ++c) {
      std::printf("%-18s", schema[c].name.c_str());
    }
    std::printf("\n");
    for (std::size_t i = 0; i < rows.rows(); ++i) {
      for (std::size_t c = 0; c < rows.width(); ++c) {
        std::printf("%-18s", rows.column(c).is_valid(i)
                                 ? rows.column(c).value_at(i).to_string().c_str()
                                 : "NULL");
      }
      std::printf("\n");
    }
  }

  const ScanStats& stats = scan.stats();
  std::printf("\nrow groups %s of %s read  (%.1f%% skipped by zone maps)\n",
              commas(stats.row_groups_read).c_str(),
              commas(stats.row_groups_total).c_str(), stats.skip_rate() * 100.0);
  std::printf("rows decoded %s\n", commas(stats.rows_read).c_str());
  return 0;
}

void usage() {
  std::printf(
      "quarry -- a columnar query engine\n\n"
      "  quarry gen   <file> <rows> [rows-per-group]   write a synthetic segment\n"
      "  quarry info  <file>                           schema, encodings, zone maps\n"
      "  quarry scan  <file> [options]                 scan, filter, group\n\n"
      "scan options:\n"
      "  --where COL OP VALUE     filter (OP is one of = != < <= > >=)\n"
      "  --no-pushdown            evaluate the filter without zone-map skipping\n"
      "  --group-by COL           group and count\n"
      "  --sum COL                add SUM(COL) to the grouping\n"
      "  --limit N                rows or groups to print (default 10)\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  try {
    if (command == "gen") return cmd_gen(argc - 2, argv + 2);
    if (command == "info") return cmd_info(argc - 2, argv + 2);
    if (command == "scan") return cmd_scan(argc - 2, argv + 2);
    if (command == "-h" || command == "--help" || command == "help") {
      usage();
      return 0;
    }
  } catch (const QuarryError& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
  usage();
  return 2;
}
