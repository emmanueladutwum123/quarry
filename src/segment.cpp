// SPDX-License-Identifier: Apache-2.0
#include "quarry/segment.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>

#include "quarry/endian.hpp"

namespace quarry {
namespace {

void append_u8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}
void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  const std::size_t at = out.size();
  out.resize(at + 4);
  store_u32_le(out.data() + at, value);
}
void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  const std::size_t at = out.size();
  out.resize(at + 8);
  store_u64_le(out.data() + at, value);
}

void append_value(std::vector<std::byte>& out, const Value& value) {
  append_u8(out, static_cast<std::uint8_t>(value.type));
  switch (value.type) {
    case TypeId::Int32: {
      const std::size_t at = out.size();
      out.resize(at + 4);
      store_i32_le(out.data() + at, value.i32);
      break;
    }
    case TypeId::Int64: {
      const std::size_t at = out.size();
      out.resize(at + 8);
      store_i64_le(out.data() + at, value.i64);
      break;
    }
    case TypeId::Double: {
      const std::size_t at = out.size();
      out.resize(at + 8);
      store_f64_le(out.data() + at, value.f64);
      break;
    }
    case TypeId::String: {
      append_u32(out, static_cast<std::uint32_t>(value.str.size()));
      const auto* chars = reinterpret_cast<const std::byte*>(value.str.data());
      out.insert(out.end(), chars, chars + value.str.size());
      break;
    }
  }
}

/// Bounds-checked cursor over the footer. A corrupt or truncated file is the one
/// input a storage engine is guaranteed to see eventually, and the difference between
/// an exception and a segfault is whether every read went through something like this.
class Cursor {
 public:
  Cursor(const std::byte* data, std::size_t bytes) : data_(data), bytes_(bytes) {}

  std::uint8_t u8() {
    require(1);
    return static_cast<std::uint8_t>(data_[at_++]);
  }
  std::uint16_t u16() {
    require(2);
    const std::uint16_t value = load_u16_le(data_ + at_);
    at_ += 2;
    return value;
  }
  std::uint32_t u32() {
    require(4);
    const std::uint32_t value = load_u32_le(data_ + at_);
    at_ += 4;
    return value;
  }
  std::uint64_t u64() {
    require(8);
    const std::uint64_t value = load_u64_le(data_ + at_);
    at_ += 8;
    return value;
  }
  std::string text(std::size_t length) {
    require(length);
    std::string value(reinterpret_cast<const char*>(data_ + at_), length);
    at_ += length;
    return value;
  }
  Value value() {
    const auto type = static_cast<TypeId>(u8());
    switch (type) {
      case TypeId::Int32: return Value::of_int32(static_cast<std::int32_t>(u32()));
      case TypeId::Int64: return Value::of_int64(static_cast<std::int64_t>(u64()));
      case TypeId::Double: {
        require(8);
        const double parsed = load_f64_le(data_ + at_);
        at_ += 8;
        return Value::of_double(parsed);
      }
      case TypeId::String: {
        const std::uint32_t length = u32();
        return Value::of_string(text(length));
      }
    }
    throw QuarryError("footer: unknown type tag");
  }

 private:
  void require(std::size_t bytes) const {
    if (at_ + bytes > bytes_) throw QuarryError("footer: truncated");
  }

  const std::byte* data_;
  std::size_t bytes_;
  std::size_t at_ = 0;
};

/// Min, max and null count over the valid rows only.
///
/// Nulls are excluded deliberately: `x > 5` is false for a null in SQL, so a chunk
/// whose only non-null values are all below 5 cannot match and should be skipped even
/// if it is full of nulls. Including them would widen the range to whatever
/// placeholder sits in the slot and prune nothing. (The frame-of-reference encoder
/// does include placeholders -- it is reproducing bytes, not answering questions.)
ZoneMap compute_zone_map(const ColumnVector& column) {
  ZoneMap zone;
  zone.row_count = column.size();
  for (std::size_t i = 0; i < column.size(); ++i) {
    if (!column.is_valid(i)) {
      ++zone.null_count;
      continue;
    }
    const Value value = column.value_at(i);
    if (!zone.has_values) {
      zone.min = value;
      zone.max = value;
      zone.has_values = true;
      continue;
    }
    if (value.compare(zone.min) < 0) zone.min = value;
    if (value.compare(zone.max) > 0) zone.max = value;
  }
  return zone;
}

}  // namespace

// ---------------------------------------------------------------------------
// SegmentWriter
// ---------------------------------------------------------------------------

SegmentWriter::SegmentWriter(const std::string& path, Schema schema,
                             std::size_t rows_per_group)
    : path_(path), schema_(std::move(schema)), rows_per_group_(rows_per_group) {
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) throw QuarryError("create " + path_ + ": " + std::strerror(errno));

  pending_.reserve(schema_.size());
  for (std::size_t i = 0; i < schema_.size(); ++i) {
    pending_.emplace_back(schema_[i].type);
  }

  std::byte header[8];
  store_u32_le(header, kSegmentMagic);
  store_u16_le(header + 4, kSegmentVersion);
  store_u16_le(header + 6, 0);
  write_bytes(header, sizeof(header));
}

SegmentWriter::~SegmentWriter() {
  if (fd_ >= 0) ::close(fd_);
}

void SegmentWriter::write_bytes(const void* data, std::size_t bytes) {
  const auto* cursor = static_cast<const char*>(data);
  std::size_t remaining = bytes;
  while (remaining > 0) {
    const ssize_t written = ::write(fd_, cursor, remaining);
    // A short write is normal, not an error: write(2) is allowed to stop early on a
    // signal or a full pipe. Treating the return value as all-or-nothing is how files
    // end up silently truncated under load.
    if (written < 0) {
      if (errno == EINTR) continue;
      throw QuarryError("write " + path_ + ": " + std::strerror(errno));
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  offset_ += bytes;
}

void SegmentWriter::append(const Batch& batch) {
  if (batch.width() != schema_.size()) {
    throw QuarryError("append: batch has " + std::to_string(batch.width()) +
                      " columns, schema has " + std::to_string(schema_.size()));
  }
  std::size_t consumed = 0;
  while (consumed < batch.rows()) {
    const std::size_t room = rows_per_group_ - pending_[0].size();
    const std::size_t take = std::min(room, batch.rows() - consumed);
    for (std::size_t c = 0; c < schema_.size(); ++c) {
      pending_[c].append_from(batch.column(c), consumed, take);
    }
    consumed += take;
    rows_written_ += take;
    if (pending_[0].size() >= rows_per_group_) flush_row_group();
  }
}

void SegmentWriter::flush_row_group() {
  if (pending_.empty() || pending_[0].empty()) return;

  RowGroupMeta group;
  group.rows = pending_[0].size();
  group.columns.resize(schema_.size());

  for (std::size_t c = 0; c < schema_.size(); ++c) {
    ColumnVector& column = pending_[c];
    column.compact_validity();

    ChunkMeta meta;
    meta.zone_map = compute_zone_map(column);

    std::vector<std::byte> encoded;
    meta.encoding = encode_best(column, encoded);
    meta.offset = offset_;
    meta.bytes = encoded.size();
    write_bytes(encoded.data(), encoded.size());

    if (!column.all_valid()) {
      const Bitmap& validity = column.validity();
      std::vector<std::byte> words(validity.word_count() * 8);
      for (std::size_t w = 0; w < validity.word_count(); ++w) {
        store_u64_le(words.data() + w * 8, validity.words()[w]);
      }
      meta.validity_offset = offset_;
      meta.validity_bytes = words.size();
      write_bytes(words.data(), words.size());
    }

    group.columns[c] = std::move(meta);
    column.clear();
  }

  groups_.push_back(std::move(group));
}

void SegmentWriter::write_footer() {
  std::vector<std::byte> footer;

  append_u32(footer, static_cast<std::uint32_t>(schema_.size()));
  for (const Field& field : schema_.fields()) {
    append_u32(footer, static_cast<std::uint32_t>(field.name.size()));
    const auto* chars = reinterpret_cast<const std::byte*>(field.name.data());
    footer.insert(footer.end(), chars, chars + field.name.size());
    append_u8(footer, static_cast<std::uint8_t>(field.type));
    append_u8(footer, field.nullable ? 1u : 0u);
  }

  append_u32(footer, static_cast<std::uint32_t>(groups_.size()));
  for (const RowGroupMeta& group : groups_) {
    append_u64(footer, group.rows);
    for (const ChunkMeta& chunk : group.columns) {
      append_u64(footer, chunk.offset);
      append_u64(footer, chunk.bytes);
      append_u8(footer, static_cast<std::uint8_t>(chunk.encoding));
      append_u64(footer, chunk.validity_offset);
      append_u64(footer, chunk.validity_bytes);
      append_u64(footer, chunk.zone_map.null_count);
      append_u64(footer, chunk.zone_map.row_count);
      append_u8(footer, chunk.zone_map.has_values ? 1u : 0u);
      if (chunk.zone_map.has_values) {
        append_value(footer, chunk.zone_map.min);
        append_value(footer, chunk.zone_map.max);
      }
    }
  }

  write_bytes(footer.data(), footer.size());

  std::byte trailer[8];
  store_u32_le(trailer, static_cast<std::uint32_t>(footer.size()));
  store_u32_le(trailer + 4, kSegmentMagic);
  write_bytes(trailer, sizeof(trailer));
}

void SegmentWriter::finish() {
  if (finished_) return;
  flush_row_group();
  write_footer();
  if (::close(fd_) != 0) {
    fd_ = -1;
    throw QuarryError("close " + path_ + ": " + std::strerror(errno));
  }
  fd_ = -1;
  finished_ = true;
}

// ---------------------------------------------------------------------------
// SegmentReader
// ---------------------------------------------------------------------------

SegmentReader SegmentReader::open(const std::string& path) {
  SegmentReader reader;
  reader.file_ = MappedFile::open(path);

  const std::byte* base = reader.file_.data();
  const std::size_t bytes = reader.file_.size();
  if (bytes < 16) throw QuarryError("segment too small: " + path);
  if (load_u32_le(base) != kSegmentMagic) {
    throw QuarryError("not a quarry segment: " + path);
  }
  if (load_u32_le(base + bytes - 4) != kSegmentMagic) {
    throw QuarryError("segment trailer missing (truncated write?): " + path);
  }

  const std::uint32_t footer_bytes = load_u32_le(base + bytes - 8);
  if (footer_bytes + 8u > bytes) throw QuarryError("footer length out of range");

  Cursor cursor(base + bytes - 8 - footer_bytes, footer_bytes);

  const std::uint32_t field_count = cursor.u32();
  std::vector<Field> fields;
  fields.reserve(field_count);
  for (std::uint32_t i = 0; i < field_count; ++i) {
    const std::uint32_t name_length = cursor.u32();
    Field field;
    field.name = cursor.text(name_length);
    field.type = static_cast<TypeId>(cursor.u8());
    field.nullable = cursor.u8() != 0;
    fields.push_back(std::move(field));
  }
  reader.schema_ = Schema(std::move(fields));

  const std::uint32_t group_count = cursor.u32();
  reader.groups_.reserve(group_count);
  for (std::uint32_t g = 0; g < group_count; ++g) {
    RowGroupMeta group;
    group.rows = cursor.u64();
    group.columns.resize(field_count);
    for (std::uint32_t c = 0; c < field_count; ++c) {
      ChunkMeta chunk;
      chunk.offset = cursor.u64();
      chunk.bytes = cursor.u64();
      chunk.encoding = static_cast<Encoding>(cursor.u8());
      chunk.validity_offset = cursor.u64();
      chunk.validity_bytes = cursor.u64();
      chunk.zone_map.null_count = cursor.u64();
      chunk.zone_map.row_count = cursor.u64();
      chunk.zone_map.has_values = cursor.u8() != 0;
      if (chunk.zone_map.has_values) {
        chunk.zone_map.min = cursor.value();
        chunk.zone_map.max = cursor.value();
      }
      if (chunk.offset + chunk.bytes > bytes) {
        throw QuarryError("chunk extends past end of file");
      }
      group.columns[c] = std::move(chunk);
    }
    reader.groups_.push_back(std::move(group));
  }
  return reader;
}

std::uint64_t SegmentReader::total_rows() const {
  std::uint64_t total = 0;
  for (const RowGroupMeta& group : groups_) total += group.rows;
  return total;
}

ColumnVector SegmentReader::read_chunk(std::size_t row_group,
                                       std::size_t column) const {
  const RowGroupMeta& group = groups_.at(row_group);
  const ChunkMeta& chunk = group.columns.at(column);
  const TypeId type = schema_[column].type;

  ColumnVector values =
      decode(file_.data() + chunk.offset, chunk.bytes, chunk.encoding, type,
             static_cast<std::size_t>(group.rows));

  if (chunk.validity_bytes > 0) {
    Bitmap validity(static_cast<std::size_t>(group.rows), true);
    const std::byte* words = file_.data() + chunk.validity_offset;
    for (std::size_t i = 0; i < group.rows; ++i) {
      const std::uint64_t word = load_u64_le(words + (i / 64) * 8);
      validity.set(i, ((word >> (i % 64)) & 1) != 0);
    }
    values.set_validity(std::move(validity));
  }
  return values;
}

Batch SegmentReader::read_row_group(std::size_t row_group,
                                    const std::vector<std::size_t>& columns) const {
  Batch batch;
  for (std::size_t column : columns) batch.add_column(read_chunk(row_group, column));
  batch.set_rows(static_cast<std::size_t>(groups_.at(row_group).rows));
  return batch;
}

}  // namespace quarry
