# duckdb-lttb

DuckDB extension implementing Largest-Triangle-Three-Buckets downsampling as aggregate functions:

```sql
lttb(x, y, n)
largestTriangleThreeBuckets(x, y, n)
```

The behavior is modeled after ClickHouse `largestTriangleThreeBuckets` / `lttb`. See `docs/reference/clickhouse-lttb.md` for the archived reference behavior and test sources.

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
