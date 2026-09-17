// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The segment file: the engine's unit of storage.
///
/// Layout, in write order:
///
///     [magic "QRRY"][u16 version][u16 flags]
///     row group 0: column 0 payload, column 1 payload, ...
///     row group 1: ...
///     footer: schema, then per row group per column
///             (offset, bytes, encoding, zone map, validity)
///     [u32 footer bytes][magic "QRRY"]
///
/// The footer is last for the same reason Parquet's is: a writer streams row groups
/// out as they fill and cannot know their offsets in advance, so metadata that
/// references them has to follow them. A reader finds it by seeking to the end,
/// reading the length, and stepping back -- two reads to learn the shape of the
/// whole file, instead of parsing forwards through gigabytes of payload.
///
/// Row group size is the central storage trade-off. Large groups mean better
/// compression ratios and fewer footer entries; small groups mean finer-grained zone
/// maps and more skipping. 65,536 rows is the default because it keeps one decoded
/// int64 column at 512KB -- comfortably a per-thread working set -- and because
/// morsel-driven parallelism needs enough groups that threads do not run out of work.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/encoding.hpp"
#include "quarry/mapped_file.hpp"
#include "quarry/schema.hpp"
#include "quarry/zone_map.hpp"

namespace quarry {

constexpr std::uint32_t kSegmentMagic = 0x59'52'52'51;  // "QRRY" little-endian
constexpr std::uint16_t kSegmentVersion = 1;
constexpr std::size_t kDefaultRowGroupRows = 65536;

struct ChunkMeta {
  std::uint64_t offset = 0;
  std::uint64_t bytes = 0;
  Encoding encoding = Encoding::Plain;
  ZoneMap zone_map;
  std::uint64_t validity_offset = 0;  ///< 0 bytes means "no nulls in this chunk"
  std::uint64_t validity_bytes = 0;
};

struct RowGroupMeta {
  std::uint64_t rows = 0;
  std::vector<ChunkMeta> columns;
};

/// Streams row groups to disk. Rows are appended a batch at a time; a row group is
/// flushed as soon as it is full, so peak memory is one row group, not one file.
class SegmentWriter {
 public:
  SegmentWriter(const std::string& path, Schema schema,
                std::size_t rows_per_group = kDefaultRowGroupRows);
  ~SegmentWriter();

  SegmentWriter(const SegmentWriter&) = delete;
  SegmentWriter& operator=(const SegmentWriter&) = delete;

  void append(const Batch& batch);

  /// Flush the final row group and write the footer. Must be called; the destructor
  /// deliberately does not do it, because a footer written during stack unwinding
  /// would turn a failed query into a file that looks complete.
  void finish();

  std::uint64_t rows_written() const { return rows_written_; }

 private:
  void flush_row_group();
  void write_bytes(const void* data, std::size_t bytes);
  void write_footer();

  std::string path_;
  Schema schema_;
  std::size_t rows_per_group_;
  int fd_ = -1;
  std::uint64_t offset_ = 0;
  std::uint64_t rows_written_ = 0;
  bool finished_ = false;

  std::vector<ColumnVector> pending_;  ///< one accumulating vector per column
  std::vector<RowGroupMeta> groups_;
};

/// Reads a segment through a memory mapping.
class SegmentReader {
 public:
  static SegmentReader open(const std::string& path);

  const Schema& schema() const { return schema_; }
  std::size_t row_group_count() const { return groups_.size(); }
  const RowGroupMeta& row_group(std::size_t index) const { return groups_[index]; }
  std::uint64_t total_rows() const;

  /// Decode one column chunk. This is the only place in the engine that turns bytes
  /// into values, which is what makes the skip decision in `can_contain` worth
  /// anything: everything above here has already been filtered by the footer.
  ColumnVector read_chunk(std::size_t row_group, std::size_t column) const;

  /// Decode several columns of one row group into a batch, in the order given.
  Batch read_row_group(std::size_t row_group,
                       const std::vector<std::size_t>& columns) const;

 private:
  MappedFile file_;
  Schema schema_;
  std::vector<RowGroupMeta> groups_;
};

}  // namespace quarry
