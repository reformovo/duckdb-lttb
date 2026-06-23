# duckdb-lttb

DuckDB extension implementing Largest-Triangle-Three-Buckets downsampling as aggregate functions:

```sql
lttb(x, y, n)
largestTriangleThreeBuckets(x, y, n)
lttb_sorted(x, y, n)
lttb_indices(x, y, n)
minmax_lttb(x, y, n, minmax_ratio)
minmax_lttb_sorted(x, y, n, minmax_ratio)
bucket_stats(x, y, num_buckets)
```

The behavior is modeled after ClickHouse `largestTriangleThreeBuckets` / `lttb`. See `docs/reference/clickhouse-lttb.md` for the archived reference behavior and test sources.

## Functions

| Function | Description |
|---|---|
| `lttb(x, y, n)` | Downsample to `n` points. Sorts input by `x` before sampling. |
| `largestTriangleThreeBuckets(x, y, n)` | Alias for `lttb`. |
| `lttb_sorted(x, y, n)` | Same as `lttb` but skips the sort. Caller must guarantee input is already ordered by `x`. |
| `lttb_indices(x, y, n)` | Same selection as `lttb` but returns `BIGINT[]` of selected sorted-position indices instead of points. |
| `minmax_lttb(x, y, n, minmax_ratio)` | Two-stage MinMax preselection + LTTB. `minmax_ratio` defaults to 4 (NULL). Reduces the LTTB triangle loop to ~`n * minmax_ratio` candidates. Approximate, not exact LTTB. |
| `minmax_lttb_sorted(x, y, n, minmax_ratio)` | Same as `minmax_lttb` but caller guarantees input is already ordered by `x`. Uses the bin-first fast path. |
| `bucket_stats(x, y, num_buckets)` | Per-bucket statistical downsampling. Returns `STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]` with y-only stats and x range boundaries. |

### Supported Input Types

Both `x` and `y` accept: `DOUBLE`, `FLOAT`, `TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `HUGEINT`, `UTINYINT`, `USMALLINT`, `UINTEGER`, `UBIGINT`, `UHUGEINT`, `DATE`, `TIMESTAMP`, `TIMESTAMP_TZ`, `TIMESTAMP_S`, `TIMESTAMP_MS`, `TIMESTAMP_NS`, `DECIMAL`.

The output preserves both `x` and `y` types: `STRUCT(x typed, y typed)[]`, matching ClickHouse `Array(Tuple(...))` semantics.

### minmax_lttb

`minmax_lttb(x, y, n, minmax_ratio)` implements the `plotly-resampler` `MinMaxLTTB` algorithm: Stage 1 divides interior points into `n * minmax_ratio / 2` equi-width x-range bins, keeping `argmin(y)` and `argmax(y)` per bin (first/last points always preserved). Stage 2 runs LTTB over the reduced candidate set.

- `minmax_ratio` defaults to 4 (pass `NULL` for the default). `minmax_ratio <= 1` is rejected.
- When `n / minmax_ratio <= n_out`, degenerates to standard LTTB (no preselect).
- Both `n` and `minmax_ratio` must be constant within each aggregate group.
- **Note**: Uses a bin-first algorithm that avoids sorting the full dataset. Instead of sort-then-preselect, it scans once for x-range, bins all points into equi-width x-bins keeping argmin/argmax of y per bin, then sorts only the ~`n * minmax_ratio` candidates before running LTTB. This eliminates the O(n log n) full sort (88% of runtime on shuffled data), achieving **7.5x speedup** over `lttb` on shuffled 1M input and **1.5x** on sorted input. Memory is still O(n) (all points accumulated in Update); the candidate set is temporary inside Finalize.
- Output preserves input types, matching `lttb`/`lttb_sorted`.

### minmax_lttb_sorted

`minmax_lttb_sorted(x, y, n, minmax_ratio)` is the sorted-input variant of `minmax_lttb`. Caller must guarantee input is already ordered by `x`. Uses the same bin-first fast path as `minmax_lttb` — since bin-first does not require sorted input, both variants achieve similar performance. The `sorted` flag is retained for API parity with `lttb_sorted`.

- Same `minmax_ratio` semantics as `minmax_lttb` (default 4, NULL for default, <= 1 rejected).
- Output preserves input types, matching `minmax_lttb`.

### bucket_stats

`bucket_stats(x, y, num_buckets)` returns per-bucket statistical summaries for distribution analysis. Useful for AI agents that need to understand data patterns, not just visual curve shape.

Output: `STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]`

- Stats are over `y` only; `x` provides bucket range boundaries (`bucket_start`, `bucket_end`).
- Equal-count-by-index bucketing (same formula as LTTB). First and last points are singletons.
- Population std (divide by N, not N-1).
- `min`/`max`/`first`/`last` preserve `y` type; `mean`/`std` are `DOUBLE`.

### ClickHouse Syntax Difference

ClickHouse uses a parametric aggregate: `lttb(n)(x, y)` where `n` is a compile-time parameter. DuckDB uses a regular aggregate: `lttb(x, y, n)` where `n` is a runtime `BIGINT` argument. The bucket count must be constant within each aggregate group.

### DuckDB Behavioral Advantages

Compared to ClickHouse and the Python `lttb` package, this extension:

- **Stable sort**: uses `stable_sort`, so duplicate `x` values preserve insertion order. ClickHouse uses unstable `::sort` (unspecified duplicate order); Python `lttb` rejects duplicate `x` entirely.
- **Internal sort**: sorts internally — no need to pre-sort input. Python `lttb` requires strictly increasing pre-sorted input and rejects unsorted/duplicate data.
- **Graceful `n < 3`**: handles `n = 0/1/2` with defined semantics. Python `lttb` raises `ValueError` for `n_out < 3`.
- **Empty next-bucket guard**: falls back to the last point when the next bucket is empty. ClickHouse source lacks this guard.
- **NULL handling**: skips `NULL` rows via the validity mask. ClickHouse has no explicit NULL handling in this function.
- **NaN**: skips `NaN` rows (same as ClickHouse); Python `lttb` rejects via validator.
- **Inf**: kept (same as ClickHouse and Python `lttb` — all three filter `NaN` but not `Inf`).

## Dependencies

This extension currently has no external runtime/build dependencies beyond the DuckDB extension template toolchain.

The template OpenSSL/vcpkg example dependency has been removed, so `vcpkg.json` is intentionally absent and `VCPKG_TOOLCHAIN_PATH` is not required.

## Building

Install `ninja` and `ccache` for faster local rebuilds:

```sh
brew install ninja ccache
```

Build with Ninja and ccache:

```sh
PATH="/opt/homebrew/opt/ccache/libexec:$PATH" GEN=ninja make
```

If an existing `build/release` directory was configured with Unix Makefiles, remove it before switching generators:

```sh
rm -rf build/release
PATH="/opt/homebrew/opt/ccache/libexec:$PATH" GEN=ninja make
```

Main build outputs:

```text
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/lttb/lttb.duckdb_extension
```

## Running

```sh
./build/release/duckdb -unsigned
```

```sql
LOAD './build/release/extension/lttb/lttb.duckdb_extension';

SELECT lttb(x, y, 1000)
FROM metrics
WHERE project_id = 'p1';
```

## Testing

Focused LTTB SQLLogicTest:

```sh
./build/release/test/unittest "test/sql/lttb.test"
```

Full extension test target:

```sh
make test
```

`make test` can feel slow because the extension template builds DuckDB and the `unittest` binary before running tests. For iteration, use the focused `unittest` command after a successful build.
