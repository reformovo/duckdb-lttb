# duckdb-lttb

DuckDB extension implementing Largest-Triangle-Three-Buckets downsampling as aggregate functions:

```sql
lttb(x, y, n)
largestTriangleThreeBuckets(x, y, n)
lttb_sorted(x, y, n)
```

The behavior is modeled after ClickHouse `largestTriangleThreeBuckets` / `lttb`. See `docs/reference/clickhouse-lttb.md` for the archived reference behavior and test sources.

## Functions

| Function | Description |
|---|---|
| `lttb(x, y, n)` | Downsample to `n` points. Sorts input by `x` before sampling. |
| `largestTriangleThreeBuckets(x, y, n)` | Alias for `lttb`. |
| `lttb_sorted(x, y, n)` | Same as `lttb` but skips the sort. Caller must guarantee input is already ordered by `x`. |

### Supported Input Types

Both `x` and `y` accept: `DOUBLE`, `FLOAT`, `TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `HUGEINT`, `UTINYINT`, `USMALLINT`, `UINTEGER`, `UBIGINT`, `UHUGEINT`, `DATE`, `TIMESTAMP`, `TIMESTAMP_TZ`, `TIMESTAMP_S`, `TIMESTAMP_MS`, `TIMESTAMP_NS`, `DECIMAL`.

The output preserves both `x` and `y` types: `STRUCT(x typed, y typed)[]`, matching ClickHouse `Array(Tuple(...))` semantics.

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
