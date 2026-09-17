# quarry

A columnar query engine in C++20, built from nothing: its own storage format, its
own encodings, its own vectorized execution engine, its own hash tables. **No third
party dependencies** — not for compression, not for testing, not for benchmarking.
`cmake && make` on a bare checkout.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/apps/quarry gen /tmp/demo.qseg 5000000
./build/apps/quarry info /tmp/demo.qseg
./build/apps/quarry scan /tmp/demo.qseg --where day '>=' 78000 --group-by mode --sum price
```

```
row groups 9 of 77 read  (88.3% skipped by zone maps)
rows decoded 543,552
```

---

## What it does

**Storage.** A column-oriented file format with per-chunk encodings — plain,
frame-of-reference bit packing, sorted dictionary, RLE — chosen per chunk by
estimating all four and keeping the smallest. Footer metadata carries a zone map
(min, max, null count) per column chunk so a scan can skip row groups it cannot
match without reading them.

**Execution.** Push-based vectorized operators: scan, filter, project, hash
aggregate, hash join, limit. Filters produce selection vectors rather than new
batches, so a conjunction evaluates its second term only on the rows the first kept.

**Measurement.** Every number below comes from `bench/`, which ships in the repo and
runs on your machine. Three of them changed the code.

## Measured (Apple M2, 8 cores, Release, medians of repeated runs)

**Encoding**, 4M rows per column:

| column | encoding | bits/row | encode | decode |
|---|---|---:|---:|---:|
| date, clustered | RLE | 1.00 | 3,962 MB/s | 898 MB/s |
| int64, random | PLAIN | 64.00 | 5,433 MB/s | 2,245 MB/s |
| price, double | DICT | 18.60 | 551 MB/s | 1,395 MB/s |
| ship mode, 7 values | DICT | 3.00 | 474 MB/s | 637 MB/s |

**Scan and filter**, 8M rows: 233M rows/s for one column unfiltered, 188M rows/s
through an integer predicate, 51M rows/s through a string equality.

**Zone-map pruning** is the largest effect in the engine and the most conditional:

| date column | row groups read | skip rate | throughput |
|---|---:|---:|---:|
| clustered (ascending) | 14 of 123 | **88.6%** | 1,227M rows/s |
| shuffled | 123 of 123 | **0.0%** | 99M rows/s |

Same predicate, same data, same index — **12.4x** apart. Pruning is a property of how
the data is laid out, not of the structure that exploits it, and an engine that
reports only the first row of that table is selling something.

**Hash aggregate**, 4M rows, as the table outgrows cache:

| groups | throughput | per row |
|---:|---:|---:|
| 8 | 56M rows/s | 17.8 ns |
| 1,024 | 36M rows/s | 27.8 ns |
| 65,536 | 28M rows/s | 35.3 ns |
| 1,000,000 | 4.7M rows/s | 212.9 ns |

A 12x collapse with no change to the hash or the probe sequence. Past ~1M groups the
table no longer fits in cache and every probe is a memory round trip.

**Hash join**, 4M probe rows against 500K build rows: 12.1M rows/s build, 9.4M rows/s
probe.

**Selection vectors on a conjunction**, 8M rows, first term keeping 2%:
**8.86 ms chained versus 36.50 ms** evaluating both terms over the full batch — 4.1x,
which is the entire argument for selection vectors in one number.

## Three things the benchmark found

**Dictionary encoding ran at 33 MB/s.** The first implementation inserted every row
into a `std::map` — a red-black node allocation and an O(log d) pointer chase per
row. That put the writer two orders of magnitude below PLAIN and meant the chooser
was selecting a format the writer could not afford to produce. Sorting a flat copy of
all n values fixed doubles (296 MB/s) but not strings (174 MB/s), because sorting
four million `string_view`s costs far more than the seven distinct values in them are
worth. The version that shipped hashes each row once to find distinct values, then
sorts only the distinct set: **519 MB/s on doubles, 463 MB/s on strings — 15.7x and
2.8x** over where it started.

**The join probe was accidentally row-oriented.** `column.hpp` states the discipline
the whole engine depends on: no operator may build a row. The probe was doing exactly
that — appending one value to every output column before moving to the next match,
paying a type switch per value and scattering writes across as many buffers as there
were columns. Splitting it into a pass that records matching index pairs and a pass
that gathers each output column in full took it from **5.7 to 9.4M rows/s**. The rule
was in the comments; the benchmark is what noticed it was being broken.

**The batch size everybody quotes is a plateau, not a peak.** 2,048 rows is the
conventional answer and the code's default. Measured, the curve is flat from 1,024 to
8,192 (34.2 / 33.7 / 33.7M rows/s) and falls off at both ends — 27.6M/s at 64 rows
where per-batch overhead dominates, 31.1M/s at 65,536 where the working set leaves
L2. The default is right, and it is right by a margin of 2%, not the margin the
folklore implies.

## Design decisions worth arguing about

**Encodings, not compression.** No LZ4, no zstd, nothing block-compressed. A general
compressor gets a better ratio and has to inflate the whole chunk before a single
value can be read. Every encoding here decodes in place, and most are *skippable*: a
run-length chunk answers `x > 5` from its runs, and a dictionary chunk evaluates a
string equality against the dictionary once rather than against every row.

**The dictionary is sorted, not first-seen.** Costs a sort of the distinct values at
write time; buys the invariant that code order matches value order, so a range
predicate can be evaluated against two code bounds instead of decoding. First-seen
order would make that impossible forever — and a file format is the one part of a
system whose decisions cannot be revised later.

**Push, not pull.** Volcano's `next()` puts a virtual call between every operator and
every tuple. Push inverts it: the scan drives, a batch crosses the whole pipeline
inside one call chain, and the per-operator cost is paid once per batch instead of
once per row.

**Open addressing, not chaining.** A chained hash table costs an unpredictable
pointer chase per probe, at exactly the sizes where the table has left cache — which
the aggregate table above shows is where the runtime already is. Keys are serialised
into one arena rather than stored as typed tuples, which is also what makes
multi-column grouping cost the same as single-column grouping.

**The file format is explicitly little-endian**, field by field, rather than memcpy of
host words. Slower to write, and it means a file written on one architecture reads
correctly on another.

## Correctness

44 tests, no test framework. The suite is mostly *differential*: the engine's answer
is compared against a brute-force pass over the same rows, not against a number typed
into the test.

The cases that exist because they produce plausible wrong answers rather than
crashes:

- A pushed-down zone-map predicate is re-evaluated exactly against the rows it
  admits. The footer gives an approximate answer over metadata; skipping the exact
  check returns rows that do not match.
- Zone maps exclude nulls from their bounds while the frame-of-reference encoder
  includes null placeholders — different jobs, and conflating them either widens
  every bound to zero or corrupts the round trip.
- `SUM` over an all-null group is `NULL`, not `0`. `COUNT(*)` counts null rows;
  `COUNT(col)` does not.
- A comparison against `NULL` is unknown, not false, so `NOT` inverts against its
  input selection rather than the batch — otherwise a `NOT` resurrects rows an
  earlier filter removed.
- Multi-column group keys carry a length prefix. Without it `("ab","c")` and
  `("a","bc")` serialise identically and two groups silently merge.
- Bit packing is checked at every width from 0 to 64, and integer encoding with
  `INT64_MIN` and `INT64_MAX` in one chunk, where a signed range subtraction
  overflows.

CI runs {Linux, macOS} × {gcc, clang} × {Debug, Release} under `-Werror`, then ASan,
UBSan and TSan.

## Layout

```
include/quarry/       types, bitmap, bit packing, columns, batches, endian helpers
  encoding.hpp        four encodings and the chooser
  segment.hpp         the file format: writer, reader, footer
  zone_map.hpp        min/max/null-count pruning
  expression.hpp      vectorized predicate and value evaluation
  exec/hash_table.hpp open-addressed table with a serialised-key arena
  exec/operator.hpp   push-based operators
bench/                every number in this README
apps/                 the `quarry` CLI
```

## Status

Storage and single-node execution work. Next: a SQL front end and a cost-based
planner, then morsel-driven parallelism across cores, then the distributed layer —
a coordinator, hash-partitioned shuffle, and joins across workers. `docs/design.md`
carries the reasoning, including what was considered and rejected.
