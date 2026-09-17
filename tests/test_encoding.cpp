// SPDX-License-Identifier: Apache-2.0
#include "quarry/encoding.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace quarry;

namespace {

/// Encode with a named encoding, decode, and require the column back value for value.
/// Round-trip is the only property that matters for a storage format: an encoding
/// that is 3x smaller and loses the low bit of every double is not an encoding.
void check_roundtrip(const ColumnVector& column, Encoding encoding, const char* what) {
  std::vector<std::byte> bytes;
  if (!encode_with(column, encoding, bytes)) return;  // not applicable to this type

  const ColumnVector back =
      decode(bytes.data(), bytes.size(), encoding, column.type(), column.size());

  if (back.size() != column.size()) {
    ::quarry::test::fail(__FILE__, __LINE__,
                         std::string(what) + "/" + encoding_name(encoding) +
                             ": size " + std::to_string(back.size()) + " != " +
                             std::to_string(column.size()));
    return;
  }
  for (std::size_t i = 0; i < column.size(); ++i) {
    bool same = false;
    switch (column.type()) {
      case TypeId::Int32: same = back.int32_at(i) == column.int32_at(i); break;
      case TypeId::Int64: same = back.int64_at(i) == column.int64_at(i); break;
      case TypeId::Double: same = back.double_at(i) == column.double_at(i); break;
      case TypeId::String: same = back.string_at(i) == column.string_at(i); break;
    }
    if (!same) {
      ::quarry::test::fail(__FILE__, __LINE__,
                           std::string(what) + "/" + encoding_name(encoding) +
                               ": row " + std::to_string(i) + " differs");
      return;
    }
  }
}

void check_all_encodings(const ColumnVector& column, const char* what) {
  for (Encoding encoding : {Encoding::Plain, Encoding::FrameOfRef, Encoding::Dictionary,
                            Encoding::Rle}) {
    check_roundtrip(column, encoding, what);
  }
}

ColumnVector make_int32(const std::vector<std::int32_t>& values) {
  ColumnVector column(TypeId::Int32);
  for (std::int32_t value : values) column.append_int32(value);
  return column;
}

}  // namespace

TEST(roundtrip_int32_random) {
  std::mt19937 rng(1);
  ColumnVector column(TypeId::Int32);
  for (int i = 0; i < 5000; ++i) {
    column.append_int32(static_cast<std::int32_t>(rng()));
  }
  check_all_encodings(column, "int32 random");
}

TEST(roundtrip_int64_narrow_range) {
  // The frame-of-reference case: large values, tiny spread. This is what a timestamp
  // column inside one row group looks like.
  std::mt19937 rng(2);
  ColumnVector column(TypeId::Int64);
  const std::int64_t base = 1'700'000'000'000LL;
  for (int i = 0; i < 4096; ++i) {
    column.append_int64(base + static_cast<std::int64_t>(rng() % 1000));
  }
  check_all_encodings(column, "int64 narrow");
}

TEST(roundtrip_doubles_including_edge_values) {
  ColumnVector column(TypeId::Double);
  for (double value : {0.0, -0.0, 1.0, -1.0, 3.141592653589793, 1e308, -1e308,
                       4.9406564584124654e-324}) {
    column.append_double(value);
  }
  std::mt19937_64 rng(3);
  for (int i = 0; i < 1000; ++i) {
    double value = 0;
    const std::uint64_t bits = rng();
    std::memcpy(&value, &bits, sizeof(value));
    if (value == value) column.append_double(value);  // skip the NaNs
  }
  check_all_encodings(column, "doubles");
}

TEST(roundtrip_strings) {
  ColumnVector column(TypeId::String);
  const char* words[] = {"AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB", "REG AIR"};
  std::mt19937 rng(4);
  for (int i = 0; i < 3000; ++i) column.append_string(words[rng() % 7]);
  column.append_string("");  // empty strings are a real value, not a null
  check_all_encodings(column, "strings");
}

TEST(roundtrip_empty_and_single_row) {
  check_all_encodings(ColumnVector(TypeId::Int32), "empty int32");
  check_all_encodings(ColumnVector(TypeId::String), "empty string");
  check_all_encodings(make_int32({42}), "single int32");
}

TEST(roundtrip_extreme_integer_range) {
  // min and max in the same chunk: the frame-of-reference range is 2^64-1, which
  // overflows a signed subtraction. If this passes, the unsigned path is right.
  check_all_encodings(make_int32({std::numeric_limits<std::int32_t>::min(),
                                  std::numeric_limits<std::int32_t>::max(), 0}),
                      "int32 extremes");
  ColumnVector wide(TypeId::Int64);
  wide.append_int64(std::numeric_limits<std::int64_t>::min());
  wide.append_int64(std::numeric_limits<std::int64_t>::max());
  wide.append_int64(0);
  check_all_encodings(wide, "int64 extremes");
}

TEST(chooser_picks_rle_for_a_sorted_low_cardinality_column) {
  ColumnVector column(TypeId::Int32);
  for (int value = 0; value < 8; ++value) {
    for (int i = 0; i < 1000; ++i) column.append_int32(value);
  }
  std::vector<std::byte> bytes;
  const Encoding chosen = encode_best(column, bytes);
  CHECK_EQ(std::string(encoding_name(chosen)), "RLE");
  // 8000 plain int32s are 32KB; 8 runs are 96 bytes plus a header.
  CHECK(bytes.size() < 200);
}

TEST(chooser_picks_dictionary_for_repeated_strings) {
  ColumnVector column(TypeId::String);
  const char* words[] = {"DELIVER IN PERSON", "COLLECT COD", "NONE", "TAKE BACK RETURN"};
  std::mt19937 rng(5);
  for (int i = 0; i < 4000; ++i) column.append_string(words[rng() % 4]);

  std::vector<std::byte> bytes;
  const Encoding chosen = encode_best(column, bytes);
  CHECK_EQ(std::string(encoding_name(chosen)), "DICT");
  CHECK(bytes.size() < estimate_encoded_size(column, Encoding::Plain) / 8);
}

TEST(chooser_picks_frame_of_reference_for_clustered_integers) {
  std::mt19937 rng(6);
  ColumnVector column(TypeId::Int64);
  for (int i = 0; i < 4096; ++i) {
    column.append_int64(1'700'000'000'000LL + static_cast<std::int64_t>(rng() % 512));
  }
  std::vector<std::byte> bytes;
  const Encoding chosen = encode_best(column, bytes);
  CHECK_EQ(std::string(encoding_name(chosen)), "FOR");
  // 9 bits per value instead of 64.
  CHECK(bytes.size() < column.size() * 2);
}

TEST(chooser_falls_back_to_plain_when_nothing_helps) {
  // High-cardinality random strings: the dictionary is as large as the data plus the
  // codes, and there are no runs. Plain has to win, and a chooser that always reaches
  // for a dictionary would make this column bigger.
  std::mt19937 rng(7);
  ColumnVector column(TypeId::String);
  for (int i = 0; i < 2000; ++i) {
    column.append_string("k" + std::to_string(rng()) + std::to_string(rng()));
  }
  std::vector<std::byte> bytes;
  const Encoding chosen = encode_best(column, bytes);
  CHECK_EQ(std::string(encoding_name(chosen)), "PLAIN");
}

TEST(estimate_matches_the_bytes_actually_written) {
  // The chooser compares estimates, so an estimate that disagrees with the encoder
  // silently picks the wrong encoding -- and nothing else would ever notice.
  std::mt19937 rng(8);
  std::vector<ColumnVector> columns;

  ColumnVector ints(TypeId::Int32);
  for (int i = 0; i < 777; ++i) ints.append_int32(static_cast<std::int32_t>(rng() % 100));
  columns.push_back(std::move(ints));

  ColumnVector runs(TypeId::Int64);
  for (int i = 0; i < 500; ++i) runs.append_int64(i / 50);
  columns.push_back(std::move(runs));

  ColumnVector words(TypeId::String);
  for (int i = 0; i < 600; ++i) words.append_string(i % 3 == 0 ? "alpha" : "beta");
  columns.push_back(std::move(words));

  for (const ColumnVector& column : columns) {
    for (Encoding encoding : {Encoding::Plain, Encoding::FrameOfRef,
                              Encoding::Dictionary, Encoding::Rle}) {
      std::vector<std::byte> bytes;
      if (!encode_with(column, encoding, bytes)) continue;
      CHECK_EQ(estimate_encoded_size(column, encoding), bytes.size());
    }
  }
}
