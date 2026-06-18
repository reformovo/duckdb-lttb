# TODO

Remaining optimization and compatibility work for `duckdb-lttb`.

## P0: Type Support

- [ ] Add direct `DATE` / `TIMESTAMP` input support
  - Do not rely on DuckDB implicit `TIMESTAMP -> DOUBLE` casts.
  - Add typed update paths that read physical `date_t` / `timestamp_t` values and convert to epoch doubles.
  - Add SQLLogicTests for direct `DATE` and direct `TIMESTAMP` input.

- [ ] Decide temporal output semantics
  - Current output is `STRUCT(x DOUBLE, y DOUBLE)[]`.
  - Decide whether to keep this simple shape or add a typed output variant closer to ClickHouse.

- [ ] Evaluate additional temporal variants
  - Consider `TIMESTAMP_S`, `TIMESTAMP_MS`, `TIMESTAMP_NS`, and `TIMESTAMP_TZ`.
  - Document unsupported variants explicitly if not implemented.

## P1: State And Memory

- [ ] Reduce per-group heap allocation
  - Current state uses `new std::vector<LTTBPoint>` per aggregate group.
  - Consider DuckDB allocator/arena integration or an inline state wrapper.

- [ ] Improve vector growth behavior
  - Explore practical `reserve` points or combine-time capacity growth.
  - Measure impact on many-group workloads.

- [ ] Add memory guard controls
  - Keep the current hard max point guard.
  - Consider user-configurable max points per group for large production queries.

## P1: Algorithm And Execution Performance

- [ ] Add sorted-input fast path
  - Provide a separate function such as `lttb_sorted(x, y, n)`.
  - Skip `stable_sort` when caller guarantees input is already ordered by `x`.

- [ ] Add duplicate-`x` bucket-selection coverage
  - Existing duplicate-`x` test covers passthrough behavior.
  - Add a case where `n < input_size` to exercise bucket selection with duplicate `x` values.

- [ ] Evaluate index-sort strategy
  - Current `LTTBPoint` is small, so sorting points is likely fine.
  - Revisit if state stores wider records or selected-index output is added.

## P2: API Extensions

- [ ] Document ClickHouse syntax difference
  - DuckDB uses `lttb(x, y, n)`.
  - ClickHouse uses `lttb(n)(x, y)` / `largestTriangleThreeBuckets(n)(x, y)`.

- [ ] Consider selected-index output
  - Add a function such as `lttb_indices(x, y, n)`.
  - Useful for frontends that want to reuse original columns without reconstructing points.

- [ ] Consider table-function output
  - Explore a row-oriented API that returns `(x, y)` rows directly.
  - This may be easier for charting queries than `LIST<STRUCT>`.

## P2: Approximate Large-Data Path

- [ ] Design `minmax_lttb` or equivalent approximate path
  - First preserve `first` / `last` / `min` / `max` per coarse bucket.
  - Then run LTTB over the reduced candidate set.
  - Document that this is approximate, not exact LTTB.

- [ ] Document DuckLake / Parquet usage patterns
  - Filter by `projectId`, `experimentId`, and `key` before downsampling.
  - Recommend partitioning by these dimensions for file pruning.

- [ ] Explore multi-resolution summary tables
  - Useful for repeated visualization queries over very large scalar metric series.

## P3: Tests, Benchmarks, And Docs

- [ ] Add more SQLLogicTests
  - Direct `DATE` / `TIMESTAMP` once implemented.
  - Duplicate `x` with `n < input_size`.
  - Parallel/group combine behavior.
  - Large `n`, zero `n`, all invalid values.

- [ ] Add benchmarks
  - Include 100K and 1M point single-series benchmarks.
  - Track sort, sampling, and finalize output costs.

- [ ] Expand documentation
  - Explain memory model: exact LTTB is not O(1) memory.
  - Explain ClickHouse parity and known differences.
  - Include DuckLake scalar-metric example queries.

## Recommended Next Step

Implement direct `DATE` / `TIMESTAMP` typed input support, then commit it as its own step with focused SQLLogicTests.
