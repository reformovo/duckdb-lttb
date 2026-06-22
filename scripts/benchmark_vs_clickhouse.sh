#!/usr/bin/env bash
# Benchmark: DuckDB lttb extension vs ClickHouse largestTriangleThreeBuckets.
#
# Compares the performance of:
#   1. DuckDB C++ lttb extension (native aggregate function)
#   2. ClickHouse largestTriangleThreeBuckets (native aggregate function)
#
# Both process the same synthetic sine-wave datasets at 10K, 100K, and 1M
# point scales, downsampling to n=100, 1000, and 10000 output points.
# Also tests shuffled input to measure sort cost.
#
# Usage:
#   ./scripts/benchmark_vs_clickhouse.sh \
#     [--duckdb ./build/release/duckdb] \
#     [--extension ./build/release/extension/lttb/lttb.duckdb_extension] \
#     [--container swanlab-clickhouse]
#
# Requires: Apple container with a running ClickHouse container.

set -euo pipefail

DUCKDB="./build/release/duckdb"
EXTENSION="./build/release/extension/lttb/lttb.duckdb_extension"
CONTAINER="swanlab-clickhouse"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duckdb)    DUCKDB="$2"; shift 2 ;;
        --extension) EXTENSION="$2"; shift 2 ;;
        --container) CONTAINER="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -x "$DUCKDB" ]; then
    echo "Error: duckdb binary not found at $DUCKDB" >&2
    exit 1
fi
if [ ! -f "$EXTENSION" ]; then
    echo "Error: extension not found at $EXTENSION" >&2
    exit 1
fi

# Verify ClickHouse container is running
if ! container list 2>/dev/null | grep -q "$CONTAINER"; then
    echo "Error: container '$CONTAINER' is not running" >&2
    exit 1
fi

CH_VERSION=$(container exec "$CONTAINER" clickhouse-client -q "SELECT version()" 2>/dev/null)
echo "ClickHouse version: $CH_VERSION"

# ---------------------------------------------------------------------------
# Timing functions
# ---------------------------------------------------------------------------

# Run DuckDB query N times, return best real time in seconds (as ms).
# $1 = setup SQL, $2 = query SQL, $3 = runs (default 5)
bench_duckdb() {
    local setup="$1"
    local query="$2"
    local runs="${3:-5}"
    local best=""

    for i in $(seq 1 $runs); do
        local t
        t=$(printf ".timer on\n%s\n%s\n" "$setup" "$query" \
            | "$DUCKDB" -unsigned 2>&1 \
            | grep "Run Time" | tail -1 | sed 's/.*real \([0-9.]*\).*/\1/')
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then
            best="$t"
        fi
    done
    # Convert to ms
    echo "$best" | awk '{printf "%.1f", $1 * 1000}'
}

# Run ClickHouse query N times, return best time in ms.
# Setup runs once (without --time); query runs N times with --time.
# $1 = setup SQL, $2 = query SQL, $3 = runs (default 5)
bench_clickhouse() {
    local setup="$1"
    local query="$2"
    local runs="${3:-5}"
    local best=""

    # Run setup once (no timing)
    container exec "$CONTAINER" clickhouse-client -q "$setup" >/dev/null 2>&1

    for i in $(seq 1 $runs); do
        local t
        # --time prints elapsed seconds to stderr; query result to stdout
        t=$(container exec "$CONTAINER" clickhouse-client --time -q "$query" 2>&1 >/dev/null \
            | tail -1 | tr -d '[:space:]')
        # Validate it's a number
        if ! echo "$t" | grep -qE '^[0-9]+\.[0-9]+$'; then
            continue
        fi
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then
            best="$t"
        fi
    done
    if [ -z "$best" ]; then
        echo "N/A"
    else
        echo "$best" | awk '{printf "%.1f", $1 * 1000}'
    fi
}

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------

echo "================================================================================"
echo "Benchmark: DuckDB lttb extension vs ClickHouse largestTriangleThreeBuckets"
echo "================================================================================"
echo "DuckDB CLI:       $DUCKDB"
echo "Extension:        $EXTENSION"
echo "ClickHouse:       $CONTAINER (v$CH_VERSION)"
echo "Date:             $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Method:           best of 5 runs"
echo ""

LOAD="LOAD '$EXTENSION';"

# Header
printf "%-22s %6s %14s %14s %10s\n" "Dataset" "n_out" "DuckDB (ms)" "ClickHouse (ms)" "Ratio"
printf "%-22s %6s %14s %14s %10s\n" "......................" "......" ".............." ".............." ".........."
echo "------------------------------------------------------------------------------------------------"

SIZES="10000 100000 1000000"
N_OUTS="100 1000 10000"

for SIZE in $SIZES; do
    for N in $N_OUTS; do
        if [ "$N" -gt "$SIZE" ]; then
            continue
        fi

        LABEL="$((SIZE / 1000))K sorted"

        # DuckDB setup + query
        DDB_SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
        DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
        DDB_TIME=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

        # ClickHouse setup + query (setup runs once, query is timed)
        CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT number::Float64 AS x, sin(number / ${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
        CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
        CH_TIME=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

        # Ratio (DuckDB / ClickHouse, <1 means DuckDB faster)
        if [ "$CH_TIME" = "N/A" ]; then
            RATIO="N/A"
        else
            RATIO=$(echo "scale=2; $DDB_TIME / $CH_TIME" | bc -l 2>/dev/null || echo "N/A")
        fi

        printf "%-22s %6s %14s %14s %10s\n" "$LABEL" "$N" "$DDB_TIME" "$CH_TIME" "${RATIO}x"
    done
done

# Shuffled input comparison
echo ""
printf "%-22s %6s %14s %14s %10s\n" "Dataset" "n_out" "DuckDB (ms)" "ClickHouse (ms)" "Ratio"
printf "%-22s %6s %14s %14s %10s\n" "......................" "......" ".............." ".............." ".........."
echo "------------------------------------------------------------------------------------------------"

for SIZE in $SIZES; do
    N=1000
    LABEL="$((SIZE / 1000))K shuffled"

    # DuckDB on shuffled input (sorts internally)
    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i)) ORDER BY random();"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    DDB_TIME=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    # ClickHouse on shuffled input (sorts internally)
    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT x, y FROM (SELECT number::Float64 AS x, sin(number / ${SIZE}.0)::Float64 AS y FROM numbers(${SIZE})) ORDER BY rand();"
    CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
    CH_TIME=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

        if [ "$CH_TIME" = "N/A" ]; then
            RATIO="N/A"
        else
            RATIO=$(echo "scale=2; $DDB_TIME / $CH_TIME" | bc -l 2>/dev/null || echo "N/A")
        fi

    printf "%-22s %6s %14s %14s %10s\n" "$LABEL" "$N" "$DDB_TIME" "$CH_TIME" "${RATIO}x"
done

# TIMESTAMP input comparison
echo ""
printf "%-22s %6s %14s %14s %10s\n" "Dataset" "n_out" "DuckDB (ms)" "ClickHouse (ms)" "Ratio"
printf "%-22s %6s %14s %14s %10s\n" "......................" "......" ".............." ".............." ".........."
echo "------------------------------------------------------------------------------------------------"

for SIZE in 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K TIMESTAMP"

    # DuckDB with TIMESTAMP input
    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT to_timestamp(i) AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    DDB_TIME=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    # ClickHouse with DateTime input
    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT toDateTime(number) AS x, sin(number / ${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
    CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
    CH_TIME=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

        if [ "$CH_TIME" = "N/A" ]; then
            RATIO="N/A"
        else
            RATIO=$(echo "scale=2; $DDB_TIME / $CH_TIME" | bc -l 2>/dev/null || echo "N/A")
        fi

    printf "%-22s %6s %14s %14s %10s\n" "$LABEL" "$N" "$DDB_TIME" "$CH_TIME" "${RATIO}x"
done

# Multi-group comparison
echo ""
printf "%-22s %6s %14s %14s %10s\n" "Dataset" "n_out" "DuckDB (ms)" "ClickHouse (ms)" "Ratio"
printf "%-22s %6s %14s %14s %10s\n" "......................" "......" ".............." ".............." ".........."
echo "------------------------------------------------------------------------------------------------"

for SIZE in 100000 1000000; do
    N=100
    NGROUPS=100
    LABEL="$((SIZE / 1000))K ${NGROUPS}groups"

    # DuckDB multi-group
    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT (i % ${NGROUPS})::INTEGER AS g, (i / ${NGROUPS})::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b GROUP BY g);"
    DDB_TIME=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    # ClickHouse multi-group
    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT (number % ${NGROUPS})::UInt32 AS g, (number / ${NGROUPS})::Float64 AS x, sin(number / ${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
    CH_QUERY="SELECT count() FROM (SELECT g, arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) AS p FROM b GROUP BY g)"
    CH_TIME=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

        if [ "$CH_TIME" = "N/A" ]; then
            RATIO="N/A"
        else
            RATIO=$(echo "scale=2; $DDB_TIME / $CH_TIME" | bc -l 2>/dev/null || echo "N/A")
        fi

    printf "%-22s %6s %14s %14s %10s\n" "$LABEL" "$N" "$DDB_TIME" "$CH_TIME" "${RATIO}x"
done

echo ""
echo "================================================================================"
echo "Notes:"
echo "  - Ratio = DuckDB / ClickHouse. <1.0 means DuckDB is faster."
echo "  - Both use native C++ aggregate functions (no Python overhead)."
echo "  - DuckDB runs via CLI subprocess; ClickHouse via container exec."
echo "  - Both include process startup + query execution in the timing."
echo "  - ClickHouse runs inside a Linux container (Apple Virtualization Framework)."
echo "  - DuckDB runs natively on macOS (arm64)."
echo "================================================================================"
