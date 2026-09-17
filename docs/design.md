# Design notes

What was chosen, what was rejected, and what the measurements said. Every number
here comes from `bench/quarry_bench` on an Apple M2 (8 cores, Release); re-run it
rather than trusting the figures.

## 1. Storage

### Encodings instead of a block compressor

A columnar file has two ways to get small: encode values compactly using what is
known about them, or compress the bytes generically. Parquet does both. quarry does
only the first, deliberately.

A block compressor gets a better ratio, and it forces every read of the chunk to
inflate the whole chunk first. That is a memcpy of the decompressed size in front of
every scan, and it destroys the two properties that make encodings worth having:

- **Decode in place.** A frame-of-reference chunk can hand out value *i* without
  touching values 0..i-1.
- **Skip entirely.** An RLE chunk can answer `x > 5` from its runs. A dictionary
  chunk can evaluate `mode = 'AIR'` against the dictionary once — seven comparisons
  — and then scan codes for one integer. Neither is possible once the bytes are
  opaque until inflated.

The cost is honest and visible in `quarry info`: a high-cardinality double column
encodes at 62.96 bits/row against a plain 64, which is nearly nothing. zstd would
halve it. That is the trade.

### The dictionary is sorted

Sorting the distinct values at write time costs one sort over *d* elements. It buys
the invariant `code(a) < code(b) ⟺ a < b`, which means a range predicate can be
evaluated against two code bounds without decoding anything.

The planner does not yet exploit this — but the file format cannot be changed later,
and first-seen ordering would foreclose it permanently. Decisions that are cheap now
and impossible later get made now.

### Zone maps exclude nulls; the encoder does not

Two structures describe the same chunk and disagree about nulls, on purpose.

A zone map answers "can this chunk contain a row matching `x > 5`". A null never
matches any comparison in SQL, so including the null placeholder (zero) in the
bounds would widen the range toward zero and stop the chunk pruning — the structure
would still be there and would no longer do anything.

The frame-of-reference encoder includes the placeholders because it is not answering
a question, it is reproducing bytes: whatever sits in a null slot has to round trip,
or the test that says "encode then decode gives back the input" cannot be written.

### Little-endian, field by field

Every integer in the format is stored with explicit shifts, and bit packing is
LSB-first into a byte stream rather than word-at-a-time. A memcpy of host words would
be faster and would produce files that are silently wrong on the other byte order.
On a little-endian host the explicit version compiles to the same instruction.

The one place that reinterprets file bytes as a host word — the byte-aligned fast
path in `bitpack::unpack` — is guarded by `if constexpr (std::endian::native == ...)`
and a big-endian machine takes the slow path and still gets the right answer.

## 2. Execution

### Push, not pull

Volcano is pull-based: `next()` all the way down, one virtual call per operator per
tuple. It composes beautifully and it is the wrong shape for a machine with caches.

Push inverts the control flow. The scan drives, and a batch travels from the bottom
of the pipeline to the top inside one call chain, staying in cache the whole way. The
per-operator dispatch cost is paid once per *batch* — 2,048 rows — instead of once
per row.

A pipeline runs until it reaches an operator that cannot emit until it has seen all
of its input: an aggregation, or the build side of a join. Those are **pipeline
breakers**. They are where memory is consumed, where spilling will have to happen,
and where parallel workers will have to synchronise. Naming them explicitly is most
of what reasoning about a plan consists of.

### Selection vectors, and when they cost

A filter could materialise a new batch containing the surviving rows. That is simple
and copies the data at every step. A selection vector instead records which rows
survived and leaves the columns alone.

The payoff is compounding conjunctions. `a < 2 AND b < 50` over 8M rows:

```
chained through the selection :    8.86 ms
both over the full batch      :   36.50 ms   (4.12x)
```

The second term sees 160K rows instead of 8M.

The cost is real: `values[selection[i]]` is a gather, and a gather does not
auto-vectorise the way a dense loop does. So "all rows selected" is a distinct state
with its own dense, null-check-free loop — which is what the first predicate of every
pipeline and every unfiltered scan actually hits.

### Batch size

Measured, filter plus aggregate over 4M rows:

| rows/batch | 64 | 256 | 1,024 | 2,048 | 8,192 | 65,536 |
|---|---:|---:|---:|---:|---:|---:|
| M rows/s | 27.6 | 30.3 | 34.2 | 33.7 | 33.7 | 31.1 |

Flat across a wide middle, worse at both ends: per-batch overhead dominates below
~256, and above ~8,192 the working set stops fitting in L2. The conventional 2,048 is
correct and the margin over its neighbours is about 2% — worth knowing before
treating it as a tuning knob.

### Hash tables: open addressing and serialised keys

Chaining costs a pointer chase per probe into memory the prefetcher cannot predict.
Linear probing keeps collisions inside the cache line that was already fetched.

The measurement that matters is not the table design in isolation but what happens as
the table grows past cache:

| groups | 8 | 1,024 | 65,536 | 1,000,000 |
|---|---:|---:|---:|---:|
| ns/row | 17.8 | 27.8 | 35.3 | 212.9 |

A 12x collapse with no change to the hash function or the probe sequence. This is the
number that decides whether an aggregation should be partitioned across threads by
hash — each worker owning a slice small enough to stay resident — rather than sharing
one table. That is how M3 will parallelise it.

Keys are serialised into one arena rather than held as typed tuples. A group key of
`(int32, string)` has no fixed size; a variant tuple would allocate per group and
compare field by field. Serialisation reduces hashing and equality to operations on a
byte range, and makes multi-column grouping cost what single-column grouping costs.

The length prefix on string keys is not decoration. Without it `("ab","c")` and
`("a","bc")` produce identical bytes, two distinct groups merge, and the query returns
a plausible wrong answer.

### What the join probe taught

The first hash-join probe walked each match and appended one value to every output
column before moving on. It was correct, it passed every test, and it violated the
rule stated at the top of `column.hpp`: no operator may build a row. A type switch
per value, and writes scattered across as many buffers as there were columns.

Splitting it into two passes — one recording matching `(probe row, build row)` index
pairs, one gathering each output column in full — took the probe from **5.7 to 9.4M
rows/s**. Nothing about the algorithm changed. The discipline was written down and
being broken, and the benchmark is what noticed.

## 3. What is not here yet

**A SQL front end and a planner.** The CLI exposes flags rather than a parser that
accepts three query shapes, because a parser that handles three shapes implies the
others work.

**Parallelism.** The design is morsel-driven: row groups are the natural morsel, and
they are independent because the format stores no cross-group state. The aggregation
table above says the aggregate must be hash-partitioned per worker rather than
shared, or it will contend exactly where it is already memory-bound.

**The distributed layer.** Coordinator, hash-partitioned shuffle, joins across
workers. The shuffle boundary is already implicit in the operator interface: a sink
that serialises its batches to a socket instead of to the next operator.

**Spilling.** Both pipeline breakers currently assume their state fits in memory.
That is a real limit, not an oversight to be hidden — a build side larger than RAM
fails rather than degrading, and the fix is partitioned spill-to-disk on the same
hash boundary the parallel version needs.

**Outer joins.** Inner only. Outer needs a match flag per build row and a second pass
over the build side, which is a different operator rather than a flag on this one.

## 4. Rejected

**A JIT.** Compiling a query to machine code closes the remaining gap between a
vectorized interpreter and hand-written code. It also brings in LLVM, a compile step
in the query's latency budget, and a debugging story where a wrong answer might be in
the generated code. The interpreter already amortises dispatch across 2,048 rows,
which is where most of the win is.

**`std::unordered_map` for the group table.** Node-based, one allocation per group,
and a pointer chase per probe. The aggregate numbers above are the argument.

**A third-party test framework.** An engine that cannot be built and tested from a
bare checkout and a compiler is an engine nobody runs.
