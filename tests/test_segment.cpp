// SPDX-License-Identifier: Apache-2.0
#include "quarry/segment.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace quarry;

namespace {

/// A segment file that deletes itself, so a failing test does not leave gigabytes
/// behind and a passing one does not depend on the previous run's leftovers.
class TempSegment {
 public:
  explicit TempSegment(const char* tag) {
    static int counter = 0;
    path_ = (std::filesystem::temp_directory_path() /
             ("quarry_" + std::string(tag) + "_" + std::to_string(++counter) + ".qseg"))
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

Schema make_schema() {
  Schema schema;
  schema.add("id", TypeId::Int64);
  schema.add("bucket", TypeId::Int32);
  schema.add("price", TypeId::Double);
  schema.add("mode", TypeId::String);
  return schema;
}

Batch make_batch(std::size_t rows, std::size_t start) {
  const char* modes[] = {"AIR", "RAIL", "SHIP", "TRUCK"};
  ColumnVector ids(TypeId::Int64);
  ColumnVector buckets(TypeId::Int32);
  ColumnVector prices(TypeId::Double);
  ColumnVector mode(TypeId::String);
  for (std::size_t i = 0; i < rows; ++i) {
    const std::size_t n = start + i;
    ids.append_int64(static_cast<std::int64_t>(n));
    buckets.append_int32(static_cast<std::int32_t>(n % 7));
    prices.append_double(static_cast<double>(n) * 1.5);
    mode.append_string(modes[n % 4]);
  }
  Batch batch;
  batch.add_column(std::move(ids));
  batch.add_column(std::move(buckets));
  batch.add_column(std::move(prices));
  batch.add_column(std::move(mode));
  return batch;
}

}  // namespace

TEST(segment_roundtrips_every_value_across_row_groups) {
  TempSegment temp("roundtrip");
  const std::size_t rows = 10'000;
  {
    SegmentWriter writer(temp.path(), make_schema(), 1024);
    // Append in awkward slices so row groups do not line up with batches: the writer
    // has to split a batch across a group boundary, which is where an off-by-one in
    // the accumulate loop would hide.
    std::size_t written = 0;
    for (std::size_t chunk : {300u, 1u, 999u, 4000u}) {
      const std::size_t take = std::min(chunk, rows - written);
      writer.append(make_batch(take, written));
      written += take;
      if (written >= rows) break;
    }
    writer.append(make_batch(rows - written, written));
    writer.finish();
    CHECK_EQ(writer.rows_written(), static_cast<std::uint64_t>(rows));
  }

  const SegmentReader reader = SegmentReader::open(temp.path());
  CHECK_EQ(reader.total_rows(), static_cast<std::uint64_t>(rows));
  CHECK_EQ(reader.schema().size(), std::size_t{4});
  CHECK(reader.row_group_count() >= 9);

  const char* modes[] = {"AIR", "RAIL", "SHIP", "TRUCK"};
  std::size_t seen = 0;
  for (std::size_t g = 0; g < reader.row_group_count(); ++g) {
    const Batch batch = reader.read_row_group(g, {0, 1, 2, 3});
    for (std::size_t i = 0; i < batch.rows(); ++i, ++seen) {
      CHECK_EQ(batch.column(0).int64_at(i), static_cast<std::int64_t>(seen));
      CHECK_EQ(batch.column(1).int32_at(i), static_cast<std::int32_t>(seen % 7));
      CHECK_EQ(batch.column(2).double_at(i), static_cast<double>(seen) * 1.5);
      CHECK_EQ(std::string(batch.column(3).string_at(i)), std::string(modes[seen % 4]));
    }
  }
  CHECK_EQ(seen, rows);
}

TEST(segment_preserves_nulls) {
  TempSegment temp("nulls");
  Schema schema;
  schema.add("value", TypeId::Int64, true);
  schema.add("label", TypeId::String, true);

  const std::size_t rows = 3000;
  {
    ColumnVector values(TypeId::Int64);
    ColumnVector labels(TypeId::String);
    for (std::size_t i = 0; i < rows; ++i) {
      if (i % 3 == 0) {
        values.append_null();
      } else {
        values.append_int64(static_cast<std::int64_t>(i) * 11);
      }
      if (i % 5 == 0) {
        labels.append_null();
      } else {
        labels.append_string("row" + std::to_string(i % 13));
      }
    }
    Batch batch;
    batch.add_column(std::move(values));
    batch.add_column(std::move(labels));

    SegmentWriter writer(temp.path(), schema, 512);
    writer.append(batch);
    writer.finish();
  }

  const SegmentReader reader = SegmentReader::open(temp.path());
  std::size_t seen = 0;
  for (std::size_t g = 0; g < reader.row_group_count(); ++g) {
    const Batch batch = reader.read_row_group(g, {0, 1});
    for (std::size_t i = 0; i < batch.rows(); ++i, ++seen) {
      CHECK_EQ(batch.column(0).is_valid(i), seen % 3 != 0);
      if (seen % 3 != 0) {
        CHECK_EQ(batch.column(0).int64_at(i), static_cast<std::int64_t>(seen) * 11);
      }
      CHECK_EQ(batch.column(1).is_valid(i), seen % 5 != 0);
      if (seen % 5 != 0) {
        CHECK_EQ(std::string(batch.column(1).string_at(i)),
                 "row" + std::to_string(seen % 13));
      }
    }
  }
  CHECK_EQ(seen, rows);
}

TEST(zone_maps_exclude_nulls_and_bound_the_real_values) {
  TempSegment temp("zones");
  Schema schema;
  schema.add("value", TypeId::Int32, true);

  {
    ColumnVector values(TypeId::Int32);
    for (int i = 0; i < 100; ++i) {
      if (i < 50) {
        values.append_null();  // half the chunk is null...
      } else {
        values.append_int32(1000 + i);  // ...and the real values are nowhere near 0
      }
    }
    Batch batch;
    batch.add_column(std::move(values));
    SegmentWriter writer(temp.path(), schema, 1024);
    writer.append(batch);
    writer.finish();
  }

  const SegmentReader reader = SegmentReader::open(temp.path());
  const ZoneMap& zone = reader.row_group(0).columns[0].zone_map;
  CHECK_EQ(zone.null_count, std::uint64_t{50});
  CHECK_EQ(zone.row_count, std::uint64_t{100});
  CHECK(zone.has_values);
  // If the null placeholder (zero) had leaked into the bounds, min would be 0 and the
  // chunk would stop pruning anything.
  CHECK_EQ(zone.min.i32, 1050);
  CHECK_EQ(zone.max.i32, 1099);
}

TEST(zone_maps_prune_the_row_groups_they_should) {
  TempSegment temp("prune");
  Schema schema;
  schema.add("day", TypeId::Int32);

  // A clustered column: each row group covers a distinct 100-day window, which is
  // what a date column in insertion order looks like.
  {
    SegmentWriter writer(temp.path(), schema, 100);
    for (int group = 0; group < 10; ++group) {
      ColumnVector days(TypeId::Int32);
      for (int i = 0; i < 100; ++i) days.append_int32(group * 100 + i);
      Batch batch;
      batch.add_column(std::move(days));
      writer.append(batch);
    }
    writer.finish();
  }

  const SegmentReader reader = SegmentReader::open(temp.path());
  CHECK_EQ(reader.row_group_count(), std::size_t{10});

  std::size_t survivors = 0;
  for (std::size_t g = 0; g < reader.row_group_count(); ++g) {
    if (reader.row_group(g).columns[0].zone_map.can_contain(CompareOp::Ge,
                                                            Value::of_int32(750))) {
      ++survivors;
    }
  }
  CHECK_EQ(survivors, std::size_t{3});  // groups 7, 8, 9

  std::size_t equality_survivors = 0;
  for (std::size_t g = 0; g < reader.row_group_count(); ++g) {
    if (reader.row_group(g).columns[0].zone_map.can_contain(CompareOp::Eq,
                                                            Value::of_int32(512))) {
      ++equality_survivors;
    }
  }
  CHECK_EQ(equality_survivors, std::size_t{1});
}

TEST(reader_rejects_a_file_that_is_not_a_segment) {
  TempSegment temp("garbage");
  std::FILE* handle = std::fopen(temp.path().c_str(), "wb");
  CHECK(handle != nullptr);
  const char junk[] = "this is not a segment file, not even close";
  std::fwrite(junk, 1, sizeof(junk), handle);
  std::fclose(handle);

  bool threw = false;
  try {
    const SegmentReader reader = SegmentReader::open(temp.path());
    (void)reader;
  } catch (const QuarryError&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(reader_rejects_a_truncated_segment) {
  TempSegment full("truncate_src");
  {
    SegmentWriter writer(full.path(), make_schema(), 256);
    writer.append(make_batch(1000, 0));
    writer.finish();
  }
  // Lop off the trailer: exactly what a crashed writer leaves behind.
  const auto size = std::filesystem::file_size(full.path());
  std::filesystem::resize_file(full.path(), size - 12);

  bool threw = false;
  try {
    const SegmentReader reader = SegmentReader::open(full.path());
    (void)reader;
  } catch (const QuarryError&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(a_single_column_read_touches_only_that_column) {
  // The columnar promise: asking for one column of a four-column file must not
  // require decoding the other three. Checked through the public API by reading one
  // column of a row group and confirming the batch has exactly one column of the
  // right values.
  TempSegment temp("projection");
  {
    SegmentWriter writer(temp.path(), make_schema(), 500);
    writer.append(make_batch(2000, 0));
    writer.finish();
  }
  const SegmentReader reader = SegmentReader::open(temp.path());
  const Batch batch = reader.read_row_group(1, {2});
  CHECK_EQ(batch.width(), std::size_t{1});
  CHECK_EQ(batch.rows(), std::size_t{500});
  CHECK_EQ(batch.column(0).double_at(0), 500.0 * 1.5);
}
