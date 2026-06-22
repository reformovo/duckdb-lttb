# TODO

Remaining optimization and compatibility work for `duckdb-lttb`.

Review basis: comparison against ClickHouse `largestTriangleThreeBuckets`
(`src/AggregateFunctions/AggregateFunctionLargestTriangleThreeBuckets.cpp`,
PR #57003, PR #84479) and the Python ecosystem (`lttb` PyPI package,
`plotly-resampler` `MinMaxLTTB`/`LTTB`, `tsdownsample`). See
`docs/reference/clickhouse-lttb.md` for the archived ClickHouse reference.

Additional review basis: comprehensive code review by @oracle (2026-06-22)
covering correctness, optimization, and feature suggestions for the PulseOn
AI Native use case (`get_metric_digest()` agent hot path).

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

## P1: Correctness Fixes (from @oracle review)

- [x] Check `Hugeint::TryCast` / `Uhugeint::TryCast` / `TryConvert` return values
  - **Issue**: `ReadAsDouble` (lines 84-93, 129-132) and `WriteFromDouble`
    (lines 176-178, 225-227) call `Hugeint::TryCast` / `TryConvert` without
    checking the boolean return value. An out-of-double-range HUGEINT would
    silently produce 0.0 instead of an error or Inf.
  - **Fix**: Check return value, throw `InvalidInputException` on failure.
    (`InvalidInputException` instead of `InternalException` so the error is
    testable via SQLLogicTest — `statement error` rejects internal errors.)
  - **Severity**: Medium (defensive programming; extremely rare in practice).
  - **Impact on PulseOn**: None (PulseOn uses step BIGINT, not HUGEINT x axis).

- [x] Add `D_ASSERT` / `std::clamp` for DECIMAL int16 write-back
  - **Issue**: `WriteFromDouble` line 215 does
    `static_cast<int16_t>(scaled)` where `scaled` is `int64_t`. If the rounded
    value exceeds `int16_t` range, this is implementation-defined behavior in
    C++17.
  - **Fix**: Clamp `scaled` to int16/int32 range via `MaxValue`/`MinValue`
    (DuckDB helpers, since `std::clamp` is unavailable in this toolchain).
    Applied to int16 and int32; int64 needs no clamp (already int64_t).
  - **Severity**: Low (LTTB only selects from existing input values; overflow
    requires boundary DECIMAL values + double round-trip ULP jitter).
  - **Impact on PulseOn**: None (PulseOn uses DOUBLE/FLOAT metric values).

- [x] Document TIMESTAMP_NS double precision limitation
  - **Issue**: `timestamp_ns_t` stores nanoseconds since epoch. At 2023 epoch
    (~1.67e18 ns), double ULP is ~256-512 ns. Two timestamps within 512ns map
    to the same double, making them indistinguishable in sort order.
  - **Resolution**: Acceptable for LTTB's visualization use case (sub-microsecond
    differences are invisible on charts). Document in README "Supported Input
    Types" section. No code change needed.
  - **Impact on PulseOn**: None (PulseOn uses step BIGINT, not TIMESTAMP_NS).

## P1: State, Memory, Performance, and Correctness

- [x] Replace raw owning pointer in `LTTBState` with arena/`unique_ptr` integration
  - Evaluated: the raw `new`/`delete` pointer pattern follows DuckDB's own
    aggregate convention (e.g. `first`/`last` uses `new char[]` with Destroy
    cleanup). Arena integration with `std::vector` is non-trivial and the
    current pattern is correct with Destroy as the designated cleanup point.
    The combine pointer-transfer optimization already improved memory behavior.
    Deferring arena integration until a clear need arises.

- [x] Add combine-time `reserve` before insert
  - `LTTBCombine` now reserves `target.size + source.size` before insert, and
    transfers the source vector pointer entirely when the target is empty (O(1)
    pointer transfer instead of copy).

- [x] Move-based combine (avoid copy entirely)
  - When target has no points, the source vector pointer is transferred (O(1)).
    Source's pointer is nulled to prevent double-free in `LTTBDestroy`.

- [x] Improve per-group vector growth during Update
  - Added a modest initial `reserve(256)` when the vector is first created,
    avoiding early geometric reallocations (0→1→2→...→256). Capped at 256 to
    avoid over-allocation in many-group scenarios.

- [ ] Add memory guard controls
  - Keep the current hard max point guard (`MAX_LTTB_POINTS`).
  - Consider user-configurable max points per group for large production queries.
  - Deferred: requires a configuration mechanism (PRAGMA setting or FunctionData)
    to pass the limit at runtime. The current hard guard of `1 << 30` is very
    generous. Revisit when production users report needing a lower cap.

- [x] Add sorted-input fast path
  - Added `lttb_sorted(x, y, n)` function that skips `stable_sort` when the
    caller guarantees input is already ordered by `x`. Uses `LTTBFunctionData`
    bind data to carry the `sorted` flag to finalize.

- [x] Evaluate index-sort strategy
  - ClickHouse sorts an index array (`PODArray<UInt32>`), not the points.
  - Current `LTTBPoint` is small (16 bytes), so sorting points is likely fine.
  - Decision: keep point-sort for now. The 16-byte struct is cache-friendly and
    the sort is not the bottleneck for typical LTTB workloads. Revisit only if
    state stores wider records or selected-index output is added.

### Algorithm Correctness

- [x] Add duplicate-`x` bucket-selection coverage
  - Added a test case with `n < input_size` and duplicate `x` values to
    exercise bucket selection (not just passthrough).

## P1: Performance Optimizations (from @oracle review)

- [ ] Hoist `ReadAsDouble` type dispatch outside the Update row loop
  - **Issue**: `ReadAsDouble(x_data, row, x_type)` is called inside the row
    loop (line 429), executing a `switch (type.id())` dispatch per row. For 1M
    rows, this is 2M switch dispatches (x + y per row).
  - **Optimization**: Resolve the type dispatch at bind time into function
    pointers (`ReadFunc`), call directly in the loop. Expected 6-12% throughput
    improvement on sorted 1M input (16ms → ~14ms).
  - **Implementation**: Generate `ReadFunc` instances per supported type (20+
    types). Use macro or template expansion. Store in `LTTBFunctionData` or a
    new bind-data struct.
  - **Priority**: High — PulseOn's `get_metric_digest()` is an agent hot path;
    cumulative savings across many queries are significant.

- [x] DECIMAL divisor/multiplier lookup table
  - **Issue**: `for (uint8_t i = 0; i < scale; i++) divisor *= 10.0` (lines
    117-119, 209-211) runs per Update call. Scale is typically 2-4, so cost is
    negligible, but for scale=38 it's 38 multiplications per row.
  - **Optimization**: Use a static `constexpr double POW10[]` lookup table
    indexed by scale (max 38 for DuckDB DECIMAL).
  - **Priority**: Low — trivial code change, near-zero runtime impact for
    typical use. Done as cleanup.

- [ ] SIMD acceleration for triangle area inner loop
  - **Issue**: The bucket candidate loop (lines 354-362) computes triangle
    areas as scalar operations. For sorted input (no sort overhead), this is
    the remaining compute bottleneck (~40-50% of the 16ms for 1M sorted points).
  - **Optimization**: Use AVX2 (x86) / NEON (ARM) to process 4 candidates
    simultaneously. Need both platform implementations + tail handling.
  - **Expected impact**: 2-4x speedup on the area computation portion (~6-8ms
    → ~2-3ms for 1M sorted).
  - **Priority**: P2 future — first land `lttb_sorted` adoption in PulseOn
    (deterministic 4.6x gain), then evaluate SIMD.

## P2: API Extensions

- [x] Document ClickHouse syntax difference
  - Documented in README.md: DuckDB uses `lttb(x, y, n)` (regular aggregate);
    ClickHouse uses `lttb(n)(x, y)` (parametric aggregate).

- [x] Document DuckDB behavioral advantages (vs ClickHouse and Python)
  - Documented in README.md: stable sort, internal sort, graceful n<3,
    empty-next-bucket guard, NULL handling, NaN/Inf behavior.

- [x] Consider selected-index output
  - Added `lttb_indices(x, y, n)` function that returns `BIGINT[]` of selected
    sorted-position indices. Useful for frontends that want to know which
    points were selected without reconstructing the full point values.

- [ ] Implement `minmax_lttb` approximate path (elevated to P1)
  - **Reference**: `plotly-resampler` `MinMaxLTTB` — min-max preselection then
    LTTB, recommended for >1M samples in interactive visualization.
  - **Why elevated**: PulseOn's auto-research workflow may encounter long
    training runs (1M+ steps per metric). Standard LTTB requires O(n) memory
    (16MB per 1M points per group). MinMaxLTTB reduces memory to O(buckets × 4)
    by pre-selecting first/last/min/max per coarse bucket, then running LTTB
    over the reduced candidate set.
  - **Proposed API**: `minmax_lttb(x, y, n, coarse_buckets)`
    - `coarse_buckets = 0`: degenerates to standard LTTB
    - `coarse_buckets >= n`: same as standard LTTB
    - Recommended: `coarse_buckets = n * 4` (plotly-resampler default)
  - **Implementation**: New aggregate state `MinMaxLTTBState` storing per-coarse-
    bucket min/max instead of all points. Or reuse `LTTBState` with a two-stage
    Finalize (MinMax preselect → LTTB). Need to handle odd-count buckets where
    min/max may be the same point.
  - **Document**: This is approximate, not exact LTTB.
  - **Status**: design only — not yet implemented.

- [ ] Implement `bucket_stats` aggregate function (new, from @oracle review)
  - **Use case**: AI agents need distribution analysis (min/max/mean/std per
    bucket), not just visual curve shape. Standard LTTB preserves shape but
    discards distribution information. Bucket statistics are more valuable for
    LLM reasoning about data patterns.
  - **Proposed API**:
    `bucket_stats(x, y, num_buckets) → STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]`
  - **Implementation**: Low complexity. No sorting or triangle computation
    needed — just linear scan + per-bucket statistical aggregation. Can reuse
    `LTTBState` (point accumulation), with a different Finalize that computes
    bucket statistics instead of LTTB selection.
  - **PulseOn mapping**: Exposed as `get_metric_digest(algorithm='bucket_stats')`
    in `AgentToolInterface`. Agents can first use `bucket_stats` to understand
    distribution, then `lttb` to see curve shape.
  - **Priority**: High — complements LTTB for agent analysis workflows.

## P3: Code Quality, Simplify, and Readability

- [x] Remove dead `next_count` ternary fallback
  - Fixed in the P0 commit: `next_count` moved inside the `if` branch, dead
    `idx_t(1)` fallback removed.

- [x] Fix `%llu` portability for `idx_t`
  - Fixed: `MAX_LTTB_POINTS` error messages now use
    `static_cast<unsigned long long>(MAX_LTTB_POINTS)` for portable `%llu`
    formatting. `MAX_LTTB_POINTS` changed to `1ULL << 30` to match ClickHouse
    verbatim.

- [x] Document `Downsample` ownership transfer
  - Added a comment documenting that `Downsample` mutates the input vector
    (sorts it) and may `std::move` it when `n <= buckets`, which is safe because
    `LTTBFinalize` is the terminal consumer.

- [x] Align `MAX_LTTB_POINTS` constant with ClickHouse
  - Changed from `idx_t(1) << 30` to `1ULL << 30` to match ClickHouse's
    `MAX_ARRAY_SIZE` verbatim.

- [x] Pin Inf handling behavior
  - Decision: keep Inf (parity with ClickHouse and Python `lttb`, which also
    filter NaN but not Inf). Inf x sorts to the extreme; Inf y can be selected
    because the triangle area becomes Inf. SQLLogicTests added for +Inf and
    -Inf x input to pin the behavior.

- [x] Document the implicit-cast input contract
  - Documented in README.md "Supported Input Types" section: the bind function
    validates x/y against the supported type set and rejects unsupported types
    (e.g. VARCHAR) with a clear error message.

## P3: Tests, Benchmarks, and Docs

- [x] Add more SQLLogicTests
  - Direct `DATE` / `TIMESTAMP` / `TIMESTAMPTZ` / `TIMESTAMP_S` / `TIMESTAMP_MS` /
    `TIMESTAMP_NS` input (P0 tests).
  - Duplicate `x` with `n < input_size`.
  - Parallel/group combine behavior (3-group test).
  - Large `n` (n > input size), zero `n`, all invalid values, Inf input.
  - `lttb_sorted` and `lttb_indices` function tests.
  - Total: 99 assertions, all pass.

- [x] Add benchmarks
  - Added `scripts/benchmark.sh` — a shell script that compares
    DuckDB lttb, ClickHouse largestTriangleThreeBuckets, and Python lttb
    on the same datasets.
  - Covers: sorted DOUBLE input (10K/100K/1M, n=100/1000/10000), shuffled
    input (sort cost), TIMESTAMP/DateTime input, multi-group (100 groups).
  - Results and analysis documented in `docs/benchmark.md`.
  - Key findings (1M points, n=1000, sorted input):
    - DuckDB: 16ms, ClickHouse: 18ms, Python: 248ms
    - DuckDB fastest in all scenarios; ClickHouse close 2nd; Python 15x slower
    - Multi-group (100 groups): DuckDB 12ms vs ClickHouse 21ms (1.75x)
    - Sort cost ~78% of total time for shuffled input at 1M scale
  - Reference bar: `plotly-resampler` uses `tsdownsample` Rust bindings with
    parallelization.

- [x] Expand documentation
  - README.md now documents: supported input types, ClickHouse syntax
    difference, DuckDB behavioral advantages, function reference table.
  - Memory model: exact LTTB is not O(1) memory — documented in the behavioral
    advantages section (state accumulates all valid points per group).
  - DuckLake scalar-metric example queries: deferred (general DuckDB guidance,
    not extension-specific).

## Items Dropped or Parked

- Dropped "Explore multi-resolution summary tables" (formerly P2): vague, no
  reference precedent; better suited to general DuckDB usage docs.
- Parked "Consider table-function output" (formerly P2): no reference
  implementation precedent; the typed-struct output path covers the primary
  need. Revisit if row-oriented API demand emerges.
- "Document DuckLake / Parquet usage patterns" (formerly P2): folded into
  "Expand documentation" above as a single example-query bullet — it is general
  DuckDB guidance, not extension-specific.
- Dropped "Separated-column output" (`lttb_split`): `UNNEST(lttb(x, y, n))`
  already provides per-row point output; Python/Polars can directly consume
  `STRUCT(x,y)[]`. Adding a separate function increases maintenance burden with
  no functional gain.
- Dropped "Weighted LTTB": no reference implementation in ClickHouse or Python
  `lttb`; no PulseOn use case. Keep LTTB as a pure equal-weight algorithm.
- Dropped "cached_digest function" (PulseOn-specific): caching logic belongs in
  the Rust `engine/digest.rs` layer, not in the DuckDB extension. Keeping the
  extension general-purpose.
- Parked "Streaming/online LTTB": LTTB is inherently off-line (needs full data
  distribution for bucketing). PulseOn's `finish_run()` one-shot downsampling
  takes 16ms for 1M points with `lttb_sorted` — acceptable. If streaming is
  needed, use `bucket_stats` for online min/max tracking during training, then
  exact LTTB at finish.
- Parked "Multi-series window-function variant": current `GROUP BY` covers the
  use case. Window-aggregate support in DuckDB's framework needs evaluation.
  Revisit if demand emerges.
- Parked "Combine multi-way merge": current `reserve + insert` is sufficient
  for PulseOn Native (single partition). Cloud version uses ClickHouse, not
  this extension. Revisit only if multi-partition combine becomes a bottleneck.

## Recommended Next Steps

The P0 type-preserving LTTB epic, most P1/P2/P3 items, and benchmarks are
complete. The remaining work, ordered by priority:

1. **`Hugeint::TryCast` return value check** (P1 correctness): add error
   handling for out-of-range HUGEINT values. Low effort, defensive programming.
2. **`ReadAsDouble` dispatch hoisting** (P1 performance): lift the type switch
   outside the Update row loop into bind-time function pointers. 6-12%
   throughput gain on the agent hot path.
3. **`minmax_lttb`** (P1, elevated from P2): implement the two-stage min-max
   preselection then LTTB approximate path for >1M point scenarios. Reference:
   `plotly-resampler`'s `MinMaxLTTB`.
4. **`bucket_stats`** (P1, new): implement bucket statistical downsampling as a
   complement to LTTB for agent distribution analysis.
5. **DECIMAL `POW10` lookup table** (P3): trivial cleanup, near-zero runtime
   impact.
6. **SIMD triangle area** (P2 future): evaluate after `lttb_sorted` adoption
   in PulseOn lands the deterministic 4.6x sort-elimination gain first.
