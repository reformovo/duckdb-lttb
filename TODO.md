# TODO

Remaining optimization and compatibility work for `duckdb-lttb`.

Review basis: comparison against ClickHouse `largestTriangleThreeBuckets`
(`src/AggregateFunctions/AggregateFunctionLargestTriangleThreeBuckets.cpp`,
PR #57003, PR #84479) and the Python ecosystem (`lttb` PyPI package,
`plotly-resampler` `MinMaxLTTB`/`LTTB`, `tsdownsample`). See
`docs/reference/clickhouse-lttb.md` for the archived ClickHouse reference.

## P0: Type-Preserving LTTB (Input + Output) — DONE

Implemented in commit following `84f1945`. The extension now accepts direct
typed input (DATE, TIMESTAMP, TIMESTAMPTZ, TIMESTAMP_S, TIMESTAMP_MS,
TIMESTAMP_NS, DECIMAL, FLOAT, SMALLINT, INTEGER, BIGINT, HUGEINT, DOUBLE) and
preserves both x and y types in the output `STRUCT(x typed, y typed)[]`,
matching ClickHouse `Array(Tuple(...))` semantics. The algorithm continues to
operate on doubles internally; type conversion happens at I/O boundaries.

- [x] Add direct `DATE` / `TIMESTAMP` typed input support
  - Typed update paths read physical `date_t` / `timestamp_t` values and convert
    to epoch doubles via `ReadAsDouble`. SQLLogicTests cover direct DATE and
    TIMESTAMP input.
  - All temporal variants (TIMESTAMPTZ, TIMESTAMP_S/MS/NS) also supported.

- [x] Implement type-preserving output path
  - Bind resolves output struct x/y types from input types. Finalize writes
    typed values via `WriteFromDouble`. SQLLogicTests assert preserved types.

- [x] Evaluate additional temporal variants
  - TIMESTAMP_S, TIMESTAMP_MS, TIMESTAMP_NS, TIMESTAMPTZ all supported.
  - TIMESTAMP_TZ is physically identical to TIMESTAMP (micros).

## P1: State, Memory, Performance, and Correctness

- [ ] Replace raw owning pointer in `LTTBState` with arena/`unique_ptr` integration
  - Current state uses `new std::vector<LTTBPoint>` per aggregate group with
    `delete` in `LTTBDestroy` (`src/lttb_extension.cpp:166-167, 200, 245`).
  - ClickHouse uses Arena allocation (`allocatesMemoryInArena() = true`).
  - Consider DuckDB `ArenaAllocator`-backed state or an inline state wrapper.

- [ ] Add combine-time `reserve` before insert
  - `LTTBCombine` (`src/lttb_extension.cpp:205`) inserts source points into the
    target vector without reserving, triggering geometric reallocation per
    combine for many partial states.
  - Add `target.points->reserve(target.points->size() + source.points->size())`
    before `insert`.

- [ ] Move-based combine (avoid copy entirely)
  - `LTTBDestroy` runs on the source state after `LTTBCombine`, so the source
    vector can be `std::move`d (or `swap`+let-destruct) into the target instead
    of copied. Correctness-safe O(1) transfer.

- [ ] Improve per-group vector growth during Update
  - Explore practical `reserve` points or combine-time capacity growth.
  - Measure impact on many-group workloads.

- [ ] Add memory guard controls
  - Keep the current hard max point guard (`MAX_LTTB_POINTS`).
  - Consider user-configurable max points per group for large production queries.

- [ ] Add sorted-input fast path
  - Provide a separate function such as `lttb_sorted(x, y, n)`.
  - Skip `stable_sort` (`src/lttb_extension.cpp:41`) when caller guarantees input
    is already ordered by `x`.

- [ ] Evaluate index-sort strategy
  - ClickHouse sorts an index array (`PODArray<UInt32>`), not the points.
  - Current `LTTBPoint` is small (16 bytes), so sorting points is likely fine.
  - Revisit if state stores wider records or selected-index output is added.

### Algorithm Correctness

- [ ] Add duplicate-`x` bucket-selection coverage
  - Existing duplicate-`x` test covers passthrough behavior (`n == input_size`).
  - Add a case where `n < input_size` to exercise bucket selection with
    duplicate `x` values.

## P2: API Extensions

- [ ] Document ClickHouse syntax difference
  - DuckDB uses `lttb(x, y, n)`.
  - ClickHouse uses `lttb(n)(x, y)` / `largestTriangleThreeBuckets(n)(x, y)`
    (parametric aggregate; `n` is a UInt64 param, not a runtime arg).

- [ ] Document DuckDB behavioral advantages (vs ClickHouse and Python)
  - Stable sort: DuckDB uses `stable_sort`, so duplicate `x` preserves insertion
    order; ClickHouse uses unstable `::sort` (unspecified duplicate order);
    Python `lttb` rejects duplicate `x` entirely.
  - Internal sort: DuckDB sorts internally; Python `lttb` requires strictly
    increasing pre-sorted input and rejects unsorted/duplicate data.
  - Graceful `n < 3`: DuckDB handles `n = 0/1/2` with defined semantics; Python
    `lttb` raises `ValueError` for `n_out < 3`.
  - Empty next-bucket guard: DuckDB falls back to the last point when the next
    bucket is empty (`src/lttb_extension.cpp:85-88`); ClickHouse source lacks
    this guard (would divide by zero — PR #57003 avoids the split in practice).
  - NULL handling: DuckDB skips NULL rows via the validity mask; ClickHouse has
    no explicit NULL handling in this function.
  - NaN: DuckDB/ClickHouse skip NaN rows; Python `lttb` rejects via validator.

- [ ] Consider selected-index output
  - Add a function such as `lttb_indices(x, y, n)`.
  - Useful for frontends that want to reuse original columns without
    reconstructing points.
  - Note: no precedent in ClickHouse or Python `lttb`; `plotly-resampler`'s
    `FuncAggregator` can return indices when `x is None`.

- [ ] Design `minmax_lttb` approximate path
  - Reference: `plotly-resampler` `MinMaxLTTB` — min-max preselection then LTTB,
    recommended for >1M samples in interactive visualization.
  - First preserve `first` / `last` / `min` / `max` per coarse bucket, then run
    LTTB over the reduced candidate set.
  - Document that this is approximate, not exact LTTB.
  - Re-evaluate priority to P1 if users report hitting the `1 << 30` point guard.

## P3: Code Quality, Simplify, and Readability

- [x] Remove dead `next_count` ternary fallback
  - Fixed in the P0 commit: `next_count` moved inside the `if` branch, dead
    `idx_t(1)` fallback removed.

- [ ] Fix `%llu` portability for `idx_t`
  - `src/lttb_extension.cpp:170, 203`: `idx_t` is `uint64_t`; `%llu` is wrong on
    LP64 Linux (`unsigned long`). Use `PRIu64` from `<cinttypes>` or DuckDB's
    type-safe formatter.

- [ ] Document `Downsample` ownership transfer
  - `src/lttb_extension.cpp:34, 48`: `Downsample` takes the state vector by
    reference and `std::move`s it when `n <= buckets`, emptying the state. This
    is correct only because `LTTBFinalize` is terminal. Add a comment or
    refactor to take by value to make ownership transfer explicit.

- [ ] Align `MAX_LTTB_POINTS` constant with ClickHouse
  - `src/lttb_extension.cpp:17`: use `1ULL << 30` to match ClickHouse's
    `MAX_ARRAY_SIZE` verbatim (currently `idx_t(1) << 30`).

- [ ] Pin Inf handling behavior
  - All three implementations (DuckDB, ClickHouse, Python `lttb`) filter NaN but
    not Inf. Inf x/y sorts to extremes and produces Inf-area triangles.
  - Decide: filter Inf (parity-plus) or document as-is (parity), then add a
    SQLLogicTest for Inf input to pin whichever behavior is chosen.
  - Reference: ClickHouse issue #64745 / PR #62646 show Inf/NaN sorting behavior
    changed across versions.

- [ ] Document the implicit-cast input contract
  - `LTTBBindFunction` (`src/lttb_extension.cpp:263-273`) resolves `ANY, ANY`
    to `DOUBLE, DOUBLE`. Non-castable types (e.g. `VARCHAR`) fail at the
    implicit-cast stage with a potentially confusing error. Document the
    accepted input contract.

## P3: Tests, Benchmarks, and Docs

- [ ] Add more SQLLogicTests
  - Direct `DATE` / `TIMESTAMP` once implemented.
  - Duplicate `x` with `n < input_size`.
  - Parallel/group combine behavior.
  - Large `n`, zero `n`, all invalid values, Inf input.

- [ ] Add benchmarks
  - Include 100K and 1M point single-series benchmarks.
  - Track sort, sampling, and finalize output costs.
  - Reference bar: `plotly-resampler` uses `tsdownsample` Rust bindings with
    parallelization.

- [ ] Expand documentation
  - Explain memory model: exact LTTB is not O(1) memory.
  - Explain ClickHouse parity and known differences (see P2 "Document DuckDB
    behavioral advantages").
  - Include DuckLake scalar-metric example queries.

## Items Dropped or Parked

- Dropped "Explore multi-resolution summary tables" (formerly P2): vague, no
  reference precedent; better suited to general DuckDB usage docs.
- Parked "Consider table-function output" (formerly P2): no reference
  implementation precedent; the typed-struct output path covers the primary
  need. Revisit if row-oriented API demand emerges.
- "Document DuckLake / Parquet usage patterns" (formerly P2): folded into
  "Expand documentation" above as a single example-query bullet — it is general
  DuckDB guidance, not extension-specific.

## Recommended Next Step

Implement the P0 type-preserving LTTB epic (direct typed input + typed output
path) as the first follow-up, with focused SQLLogicTests for direct `DATE` and
`TIMESTAMP` input and asserted output types. Then batch the P1 combine
performance fixes (reserve + move, plus the sorted fast path) and the P3
code-quality cleanups (`next_count`, `%llu`, `MAX_LTTB_POINTS`, ownership
comment) into a single mechanical follow-up PR.
