// SPDX-License-Identifier: Apache-2.0
#pragma once

/// Push-based operators: each one hands its output to the next rather than being
/// pulled from.
///
/// The classic Volcano design is pull-based -- every operator has `next()` and the
/// top of the plan drives. It composes beautifully and it puts a virtual call plus a
/// branch between every operator and every tuple. Push inverts it: the scan drives,
/// and a batch travels from the bottom of the pipeline to the top inside one call
/// chain, staying in cache the whole way. That is the layout Hyper and Photon use
/// and the reason a pipeline of five operators costs barely more than one.
///
/// A pipeline runs until it hits an operator that cannot emit until it has seen
/// everything -- an aggregation, or the build side of a join. Those are pipeline
/// breakers, they are where memory is consumed and where parallelism has to
/// synchronise, and naming them explicitly is most of what plan reasoning is.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/exec/hash_table.hpp"
#include "quarry/expression.hpp"
#include "quarry/segment.hpp"
#include "quarry/selection.hpp"

namespace quarry {

class Sink {
 public:
  virtual ~Sink() = default;
  /// Consume the rows of `batch` named by `selection`.
  virtual void consume(const Batch& batch, const Selection& selection) = 0;
  /// No more input is coming. Pipeline breakers emit their results here.
  virtual void finish() {}
};

/// Applies a predicate and forwards the survivors, without copying any data: only
/// the selection narrows.
class FilterOp : public Sink {
 public:
  FilterOp(ExprPtr predicate, Sink* output)
      : predicate_(std::move(predicate)), output_(output) {}

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override { output_->finish(); }

  std::uint64_t rows_in() const { return rows_in_; }
  std::uint64_t rows_out() const { return rows_out_; }

 private:
  ExprPtr predicate_;
  Sink* output_;
  std::uint64_t rows_in_ = 0;
  std::uint64_t rows_out_ = 0;
};

/// Materialises a list of expressions into a new batch. This is the point where a
/// selection stops being free -- the gather happens here -- so the planner pushes
/// projection as late as it can.
class ProjectOp : public Sink {
 public:
  ProjectOp(std::vector<ExprPtr> expressions, Sink* output)
      : expressions_(std::move(expressions)), output_(output) {}

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override { output_->finish(); }

 private:
  std::vector<ExprPtr> expressions_;
  Sink* output_;
};

/// Stops the pipeline once `limit` rows have been produced.
class LimitOp : public Sink {
 public:
  LimitOp(std::size_t limit, Sink* output) : limit_(limit), output_(output) {}

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override { output_->finish(); }
  bool satisfied() const { return produced_ >= limit_; }

 private:
  std::size_t limit_;
  Sink* output_;
  std::size_t produced_ = 0;
};

/// Terminal sink: keeps everything it is given. Used by tests, the CLI, and the
/// build side of a join.
class CollectSink : public Sink {
 public:
  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override {}

  const std::vector<Batch>& batches() const { return batches_; }
  std::size_t total_rows() const;
  /// All batches concatenated, for callers that want one table.
  Batch materialize() const;

 private:
  std::vector<Batch> batches_;
};

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

enum class AggFunc : std::uint8_t { Count, Sum, Min, Max, Avg };

struct AggSpec {
  AggFunc func = AggFunc::Count;
  std::size_t input_column = 0;  ///< ignored for COUNT(*)
  TypeId input_type = TypeId::Int64;
  bool count_star = false;
  std::string name;
};

constexpr const char* agg_name(AggFunc func) {
  switch (func) {
    case AggFunc::Count: return "count";
    case AggFunc::Sum: return "sum";
    case AggFunc::Min: return "min";
    case AggFunc::Max: return "max";
    case AggFunc::Avg: return "avg";
  }
  return "?";
}

/// Grouped aggregation. A pipeline breaker: nothing comes out until `finish()`.
///
/// State lives in one flat array per aggregate indexed by the dense group id the
/// hash table hands back, rather than in a struct per group. Two reasons: updating
/// SUM across a batch then touches one contiguous array instead of striding over
/// group records, and adding an aggregate does not change the table's memory layout.
class HashAggregate : public Sink {
 public:
  HashAggregate(std::vector<std::size_t> key_columns, std::vector<TypeId> key_types,
                std::vector<AggSpec> aggregates);

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override {}

  /// Group columns followed by aggregate columns.
  Batch result() const;
  std::size_t group_count() const { return table_.size(); }

 private:
  struct State {
    std::int64_t i64 = 0;
    double f64 = 0.0;
    std::uint64_t count = 0;
    bool seen = false;
  };

  std::vector<std::size_t> key_columns_;
  std::vector<TypeId> key_types_;
  std::vector<AggSpec> aggregates_;
  GroupHashTable table_;
  std::vector<std::vector<State>> state_;  ///< [aggregate][group]
};

// ---------------------------------------------------------------------------
// Hash join
// ---------------------------------------------------------------------------

/// Shared state between the two sides of a join. The build side fills it; the probe
/// side reads it and never writes, which is what makes the probe parallelisable
/// without a lock once the build has completed.
struct JoinHashIndex {
  Batch build_rows;                    ///< every build-side row, materialised
  GroupHashTable table;                ///< key -> bucket id
  std::vector<std::uint32_t> head;     ///< bucket id -> first row, or kNoRow
  std::vector<std::uint32_t> next;     ///< row -> next row with the same key

  static constexpr std::uint32_t kNoRow = 0xFFFFFFFFu;
};

/// Build side: materialises rows and indexes them by key.
class HashJoinBuild : public Sink {
 public:
  HashJoinBuild(std::vector<std::size_t> key_columns, JoinHashIndex* index)
      : key_columns_(std::move(key_columns)), index_(index) {}

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override;

 private:
  std::vector<std::size_t> key_columns_;
  JoinHashIndex* index_;
  bool initialised_ = false;
};

/// Probe side: for each probe row, emit one output row per matching build row.
/// Inner join only -- outer joins need a match flag per build row and a second pass,
/// which is a different operator rather than a flag on this one.
class HashJoinProbe : public Sink {
 public:
  HashJoinProbe(std::vector<std::size_t> key_columns, const JoinHashIndex* index,
                Sink* output)
      : key_columns_(std::move(key_columns)), index_(index), output_(output) {}

  void consume(const Batch& batch, const Selection& selection) override;
  void finish() override { output_->finish(); }

  std::uint64_t rows_probed() const { return rows_probed_; }
  std::uint64_t rows_emitted() const { return rows_emitted_; }

 private:
  std::vector<std::size_t> key_columns_;
  const JoinHashIndex* index_;
  Sink* output_;
  std::uint64_t rows_probed_ = 0;
  std::uint64_t rows_emitted_ = 0;
};

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

/// Counters a scan fills in as it runs. Reported by the CLI and asserted by tests:
/// a pruning optimisation that is not measured is an optimisation that silently
/// stops working.
struct ScanStats {
  std::uint64_t row_groups_total = 0;
  std::uint64_t row_groups_read = 0;
  std::uint64_t rows_read = 0;
  std::uint64_t rows_emitted = 0;

  double skip_rate() const {
    return row_groups_total == 0
               ? 0.0
               : 1.0 - static_cast<double>(row_groups_read) /
                           static_cast<double>(row_groups_total);
  }
};

/// Reads a segment, skipping row groups the zone maps rule out, and pushes batches
/// into a sink.
///
/// The pushed-down predicate is evaluated twice by design: once against the footer
/// to decide whether to read a row group at all, and once against the rows that
/// survive. The first is the cheap approximate answer over metadata; the second is
/// the exact one. Skipping the second because the first said "maybe" is the classic
/// way to return rows that do not match.
class TableScan {
 public:
  TableScan(const SegmentReader& reader, std::vector<std::size_t> columns)
      : reader_(reader), columns_(std::move(columns)) {}

  /// Restrict to row groups whose zone map admits `column <op> literal`.
  void push_down(std::size_t column, CompareOp op, Value literal);

  void run(Sink& sink);

  const ScanStats& stats() const { return stats_; }

 private:
  const SegmentReader& reader_;
  std::vector<std::size_t> columns_;
  struct Pushdown {
    std::size_t column;
    CompareOp op;
    Value literal;
  };
  std::vector<Pushdown> pushdowns_;
  ScanStats stats_;
};

}  // namespace quarry
