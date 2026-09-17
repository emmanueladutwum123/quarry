// SPDX-License-Identifier: Apache-2.0
#include "quarry/exec/operator.hpp"

#include <algorithm>
#include <limits>

#include "quarry/endian.hpp"

namespace quarry {
namespace {

void append_bytes(std::vector<std::byte>& out, const void* data, std::size_t bytes) {
  const auto* source = static_cast<const std::byte*>(data);
  out.insert(out.end(), source, source + bytes);
}

}  // namespace

void serialize_key(const Batch& batch, const std::vector<std::size_t>& key_columns,
                   std::size_t row, std::vector<std::byte>& out) {
  out.clear();
  for (std::size_t column_index : key_columns) {
    const ColumnVector& column = batch.column(column_index);
    // A null key is a distinct group, and it must not serialise to the same bytes as
    // a real value that happens to be zero. One tag byte separates them.
    if (!column.is_valid(row)) {
      out.push_back(std::byte{0});
      continue;
    }
    out.push_back(std::byte{1});
    switch (column.type()) {
      case TypeId::Int32: {
        std::byte buffer[4];
        store_i32_le(buffer, column.int32_at(row));
        append_bytes(out, buffer, sizeof(buffer));
        break;
      }
      case TypeId::Int64: {
        std::byte buffer[8];
        store_i64_le(buffer, column.int64_at(row));
        append_bytes(out, buffer, sizeof(buffer));
        break;
      }
      case TypeId::Double: {
        std::byte buffer[8];
        store_f64_le(buffer, column.double_at(row));
        append_bytes(out, buffer, sizeof(buffer));
        break;
      }
      case TypeId::String: {
        const std::string_view text = column.string_at(row);
        std::byte length[4];
        store_u32_le(length, static_cast<std::uint32_t>(text.size()));
        append_bytes(out, length, sizeof(length));
        append_bytes(out, text.data(), text.size());
        break;
      }
    }
  }
}

namespace {

/// Read a key back out of its serialised form, to rebuild the group columns.
void deserialize_key(std::string_view key, const std::vector<TypeId>& types,
                     std::vector<ColumnVector>& columns) {
  const auto* cursor = reinterpret_cast<const std::byte*>(key.data());
  for (std::size_t i = 0; i < types.size(); ++i) {
    const bool present = static_cast<std::uint8_t>(*cursor++) != 0;
    if (!present) {
      columns[i].append_null();
      continue;
    }
    switch (types[i]) {
      case TypeId::Int32:
        columns[i].append_int32(load_i32_le(cursor));
        cursor += 4;
        break;
      case TypeId::Int64:
        columns[i].append_int64(load_i64_le(cursor));
        cursor += 8;
        break;
      case TypeId::Double:
        columns[i].append_double(load_f64_le(cursor));
        cursor += 8;
        break;
      case TypeId::String: {
        const std::uint32_t length = load_u32_le(cursor);
        cursor += 4;
        columns[i].append_string(
            std::string_view(reinterpret_cast<const char*>(cursor), length));
        cursor += length;
        break;
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Simple operators
// ---------------------------------------------------------------------------

void FilterOp::consume(const Batch& batch, const Selection& selection) {
  rows_in_ += selection.size();
  const Selection kept = evaluate_predicate(*predicate_, batch, selection);
  rows_out_ += kept.size();
  if (!kept.empty()) output_->consume(batch, kept);
}

void ProjectOp::consume(const Batch& batch, const Selection& selection) {
  Batch out;
  for (const ExprPtr& expression : expressions_) {
    out.add_column(evaluate_value(*expression, batch, selection));
  }
  out.set_rows(selection.size());
  if (out.rows() > 0) output_->consume(out, Selection::all(out.rows()));
}

void LimitOp::consume(const Batch& batch, const Selection& selection) {
  if (produced_ >= limit_) return;
  const std::size_t room = limit_ - produced_;
  if (selection.size() <= room) {
    produced_ += selection.size();
    output_->consume(batch, selection);
    return;
  }
  std::vector<std::uint32_t> truncated;
  truncated.reserve(room);
  for (std::size_t i = 0; i < room; ++i) truncated.push_back(selection[i]);
  produced_ = limit_;
  output_->consume(batch, Selection::of(std::move(truncated)));
}

void CollectSink::consume(const Batch& batch, const Selection& selection) {
  Batch copy;
  for (std::size_t c = 0; c < batch.width(); ++c) {
    ColumnVector column(batch.column(c).type());
    column.reserve(selection.size());
    column.append_from_selection(batch.column(c), selection);
    copy.add_column(std::move(column));
  }
  copy.set_rows(selection.size());
  batches_.push_back(std::move(copy));
}

std::size_t CollectSink::total_rows() const {
  std::size_t total = 0;
  for (const Batch& batch : batches_) total += batch.rows();
  return total;
}

Batch CollectSink::materialize() const {
  Batch out;
  if (batches_.empty()) return out;
  for (std::size_t c = 0; c < batches_[0].width(); ++c) {
    ColumnVector column(batches_[0].column(c).type());
    for (const Batch& batch : batches_) {
      column.append_from(batch.column(c), 0, batch.rows());
    }
    out.add_column(std::move(column));
  }
  out.set_rows(total_rows());
  return out;
}

// ---------------------------------------------------------------------------
// HashAggregate
// ---------------------------------------------------------------------------

HashAggregate::HashAggregate(std::vector<std::size_t> key_columns,
                             std::vector<TypeId> key_types,
                             std::vector<AggSpec> aggregates)
    : key_columns_(std::move(key_columns)),
      key_types_(std::move(key_types)),
      aggregates_(std::move(aggregates)) {
  state_.resize(aggregates_.size());
}

void HashAggregate::consume(const Batch& batch, const Selection& selection) {
  std::vector<std::byte> key;
  for (std::size_t i = 0; i < selection.size(); ++i) {
    const std::size_t row = selection[i];
    serialize_key(batch, key_columns_, row, key);

    bool inserted = false;
    const std::uint32_t group =
        table_.find_or_insert(key.data(), static_cast<std::uint32_t>(key.size()),
                              inserted);
    if (inserted) {
      for (std::vector<State>& states : state_) states.emplace_back();
    }

    for (std::size_t a = 0; a < aggregates_.size(); ++a) {
      const AggSpec& spec = aggregates_[a];
      State& accumulator = state_[a][group];

      if (spec.count_star) {
        ++accumulator.count;
        accumulator.seen = true;
        continue;
      }

      const ColumnVector& column = batch.column(spec.input_column);
      // SQL aggregates ignore nulls, and COUNT(column) counts non-nulls only --
      // which is why COUNT(*) is a separate flag rather than COUNT of some column.
      if (!column.is_valid(row)) continue;

      ++accumulator.count;
      const bool first = !accumulator.seen;
      accumulator.seen = true;

      switch (column.type()) {
        case TypeId::Double: {
          const double value = column.double_at(row);
          accumulator.f64 = first ? value
                            : spec.func == AggFunc::Min ? std::min(accumulator.f64, value)
                            : spec.func == AggFunc::Max ? std::max(accumulator.f64, value)
                                                        : accumulator.f64 + value;
          break;
        }
        case TypeId::Int32:
        case TypeId::Int64: {
          const std::int64_t value = column.type() == TypeId::Int32
                                         ? static_cast<std::int64_t>(column.int32_at(row))
                                         : column.int64_at(row);
          accumulator.i64 = first ? value
                            : spec.func == AggFunc::Min ? std::min(accumulator.i64, value)
                            : spec.func == AggFunc::Max ? std::max(accumulator.i64, value)
                                                        : accumulator.i64 + value;
          break;
        }
        case TypeId::String:
          // MIN/MAX over strings would need the key arena; COUNT is the only
          // aggregate defined for them here, and it is already handled above.
          break;
      }
    }
  }
}

Batch HashAggregate::result() const {
  std::vector<ColumnVector> group_columns;
  group_columns.reserve(key_types_.size());
  for (TypeId type : key_types_) group_columns.emplace_back(type);

  std::vector<ColumnVector> agg_columns;
  agg_columns.reserve(aggregates_.size());
  for (const AggSpec& spec : aggregates_) {
    const bool integral = spec.func == AggFunc::Count ||
                          (spec.input_type != TypeId::Double && spec.func != AggFunc::Avg);
    agg_columns.emplace_back(integral ? TypeId::Int64 : TypeId::Double);
  }

  // Emit in group-id order so the output is deterministic. A hash table's slot order
  // depends on capacity and insertion history, which would make results differ run to
  // run and tests flap.
  auto entries = table_.entries();
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  for (const auto& [group, key] : entries) {
    deserialize_key(key, key_types_, group_columns);
    for (std::size_t a = 0; a < aggregates_.size(); ++a) {
      const AggSpec& spec = aggregates_[a];
      const State& accumulator = state_[a][group];
      switch (spec.func) {
        case AggFunc::Count:
          agg_columns[a].append_int64(static_cast<std::int64_t>(accumulator.count));
          break;
        case AggFunc::Avg:
          if (accumulator.count == 0) {
            agg_columns[a].append_null();
          } else {
            const double total = spec.input_type == TypeId::Double
                                     ? accumulator.f64
                                     : static_cast<double>(accumulator.i64);
            agg_columns[a].append_double(total /
                                         static_cast<double>(accumulator.count));
          }
          break;
        case AggFunc::Sum:
        case AggFunc::Min:
        case AggFunc::Max:
          if (!accumulator.seen) {
            agg_columns[a].append_null();  // every input row was null
          } else if (spec.input_type == TypeId::Double) {
            agg_columns[a].append_double(accumulator.f64);
          } else {
            agg_columns[a].append_int64(accumulator.i64);
          }
          break;
      }
    }
  }

  Batch out;
  for (ColumnVector& column : group_columns) out.add_column(std::move(column));
  for (ColumnVector& column : agg_columns) out.add_column(std::move(column));
  out.set_rows(entries.size());
  return out;
}

// ---------------------------------------------------------------------------
// Hash join
// ---------------------------------------------------------------------------

void HashJoinBuild::consume(const Batch& batch, const Selection& selection) {
  if (!initialised_) {
    for (std::size_t c = 0; c < batch.width(); ++c) {
      index_->build_rows.add_column(ColumnVector(batch.column(c).type()));
    }
    initialised_ = true;
  }
  for (std::size_t c = 0; c < batch.width(); ++c) {
    index_->build_rows.mutable_column(c).append_from_selection(batch.column(c),
                                                               selection);
  }
  index_->build_rows.set_rows(index_->build_rows.column(0).size());
}

void HashJoinBuild::finish() {
  const Batch& rows = index_->build_rows;
  const std::size_t row_count = rows.rows();

  // Chained buckets in two flat arrays rather than a vector per key: a join on a
  // column with 6M distinct values would otherwise allocate 6M vectors, and the
  // allocator becomes the join.
  index_->next.assign(row_count, JoinHashIndex::kNoRow);
  index_->head.clear();
  index_->table.reset(std::max<std::size_t>(16, row_count * 2));

  std::vector<std::byte> key;
  for (std::size_t row = 0; row < row_count; ++row) {
    serialize_key(rows, key_columns_, row, key);
    bool inserted = false;
    const std::uint32_t bucket =
        index_->table.find_or_insert(key.data(), static_cast<std::uint32_t>(key.size()),
                                     inserted);
    if (inserted) index_->head.push_back(JoinHashIndex::kNoRow);
    // Push-front: the chain ends up in reverse build order, which no caller may
    // depend on -- SQL joins are unordered, and the tests sort before comparing.
    index_->next[row] = index_->head[bucket];
    index_->head[bucket] = static_cast<std::uint32_t>(row);
  }
}

void HashJoinProbe::consume(const Batch& batch, const Selection& selection) {
  const Batch& build = index_->build_rows;

  // Two passes, because one pass is row-at-a-time.
  //
  // The obvious loop walks each match and appends one value to every output column
  // before moving on, which reintroduces exactly the row-oriented access this engine
  // exists to avoid: a switch on the column type per value, and a write to as many
  // different buffers as there are columns. Measured at 5.7M probe rows/s.
  //
  // Instead the first pass records only the matching (probe row, build row) index
  // pairs, and the second gathers each output column in full before starting the
  // next. The type switch is then hoisted out of the inner loop and each output
  // buffer is written sequentially.
  std::vector<std::uint32_t> probe_rows;
  std::vector<std::uint32_t> build_rows;
  probe_rows.reserve(selection.size());
  build_rows.reserve(selection.size());

  std::vector<std::byte> key;
  for (std::size_t i = 0; i < selection.size(); ++i) {
    const std::size_t row = selection[i];
    ++rows_probed_;
    serialize_key(batch, key_columns_, row, key);
    const std::uint32_t bucket =
        index_->table.find(key.data(), static_cast<std::uint32_t>(key.size()));
    if (bucket == GroupHashTable::kEmpty) continue;

    for (std::uint32_t build_row = index_->head[bucket];
         build_row != JoinHashIndex::kNoRow; build_row = index_->next[build_row]) {
      probe_rows.push_back(static_cast<std::uint32_t>(row));
      build_rows.push_back(build_row);
    }
  }

  if (probe_rows.empty()) return;
  rows_emitted_ += probe_rows.size();

  const Selection probe_gather = Selection::of(std::move(probe_rows));
  const Selection build_gather = Selection::of(std::move(build_rows));
  const std::size_t emitted = probe_gather.size();

  Batch out;
  for (std::size_t c = 0; c < batch.width(); ++c) {
    ColumnVector column(batch.column(c).type());
    column.reserve(emitted);
    column.append_from_selection(batch.column(c), probe_gather);
    out.add_column(std::move(column));
  }
  for (std::size_t c = 0; c < build.width(); ++c) {
    ColumnVector column(build.column(c).type());
    column.reserve(emitted);
    column.append_from_selection(build.column(c), build_gather);
    out.add_column(std::move(column));
  }

  out.set_rows(emitted);
  output_->consume(out, Selection::all(emitted));
}

// ---------------------------------------------------------------------------
// TableScan
// ---------------------------------------------------------------------------

void TableScan::push_down(std::size_t column, CompareOp op, Value literal) {
  pushdowns_.push_back(Pushdown{column, op, std::move(literal)});
}

void TableScan::run(Sink& sink) {
  stats_ = ScanStats{};
  stats_.row_groups_total = reader_.row_group_count();

  for (std::size_t g = 0; g < reader_.row_group_count(); ++g) {
    const RowGroupMeta& group = reader_.row_group(g);

    bool possible = true;
    for (const Pushdown& pushdown : pushdowns_) {
      if (!group.columns[pushdown.column].zone_map.can_contain(pushdown.op,
                                                               pushdown.literal)) {
        possible = false;
        break;
      }
    }
    if (!possible) continue;

    ++stats_.row_groups_read;
    const Batch batch = reader_.read_row_group(g, columns_);
    stats_.rows_read += batch.rows();
    stats_.rows_emitted += batch.rows();
    sink.consume(batch, Selection::all(batch.rows()));
  }
  sink.finish();
}

}  // namespace quarry
