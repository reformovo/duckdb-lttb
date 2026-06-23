#!/usr/bin/env bash
# Benchmark for the DuckDB lttb extension.
#
# Part A (sections 1-4): Three-way comparison DuckDB vs ClickHouse vs Python lttb.
#   Requires: ClickHouse container running, Python duckdb+lttb+numpy installed.
#
# Part B (sections 5-10): DuckDB-internal function-level benchmark.
#   Tests all registered functions (lttb, lttb_sorted, lttb_indices,
#   minmax_lttb, bucket_stats). Only requires DuckDB + the extension.
#
# If the ClickHouse container is not running, Part A is skipped and only
# Part B runs.
#
# Timing method:
#   - DuckDB:   CLI .timer (includes subprocess startup)
#   - ClickHouse: clickhouse-client --time (server-side, excludes connection)
#   - Python:   Python time.perf_counter (in-process, excludes DuckDB fetch)
#
# Usage:
#   ./scripts/benchmark.sh \
#     [--duckdb ./build/release/duckdb] \
#     [--extension ./build/release/extension/lttb/lttb.duckdb_extension] \
#     [--container swanlab-clickhouse]

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

if [ ! -x "$DUCKDB" ]; then echo "Error: duckdb not found at $DUCKDB" >&2; exit 1; fi
if [ ! -f "$EXTENSION" ]; then echo "Error: extension not found at $EXTENSION" >&2; exit 1; fi

# ClickHouse is optional — Part A is skipped if the container is not running.
HAS_CLICKHOUSE=false
if container list 2>/dev/null | grep -q "$CONTAINER"; then
    HAS_CLICKHOUSE=true
    CH_VERSION=$(container exec "$CONTAINER" clickhouse-client -q "SELECT version()" 2>/dev/null)
fi

LOAD="LOAD '$EXTENSION';"

# --- Timing helpers ---

# DuckDB: best of 5, returns ms (includes CLI startup + query)
bench_duckdb() {
    local setup="$1" query="$2" best="" t
    for i in $(seq 1 5); do
        t=$(printf ".timer on\n%s\n%s\n" "$setup" "$query" | "$DUCKDB" -unsigned 2>&1 \
            | grep "Run Time" | tail -1 | sed 's/.*real \([0-9.]*\).*/\1/')
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best="$t"; fi
    done
    echo "$best" | awk '{printf "%.1f", $1 * 1000}'
}

# ClickHouse: best of 5, returns ms (server-side --time, excludes connection)
bench_clickhouse() {
    local setup="$1" query="$2" best="" t
    container exec "$CONTAINER" clickhouse-client -q "$setup" >/dev/null 2>&1
    for i in $(seq 1 5); do
        t=$(container exec "$CONTAINER" clickhouse-client --time -q "$query" 2>&1 >/dev/null \
            | tail -1 | tr -d '[:space:]')
        echo "$t" | grep -qE '^[0-9]+\.[0-9]+$' || continue
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best="$t"; fi
    done
    [ -z "$best" ] && echo "N/A" || echo "$best" | awk '{printf "%.1f", $1 * 1000}'
}

# Python: best of 5, returns ms (in-process perf_counter)
bench_python() {
    local size="$1" n_out="$2" sorted="$3"
    python3 -c "
import duckdb, lttb, numpy as np, time
con = duckdb.connect()
size, n_out, sorted_in = $size, $n_out, '$sorted'
if sorted_in == 'sorted':
    q = f'SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i) ORDER BY x'
else:
    q = f'SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i)'
best = None
for _ in range(6):
    s = time.perf_counter()
    data = con.execute(q).fetchall()
    arr = np.array(data, dtype=float)
    r = lttb.downsample(arr, n_out=n_out)
    e = time.perf_counter() - s
    if best is None or e < best: best = e
print(f'{best * 1000:.1f}')
con.close()
"
}

# --- Output ---

echo "================================================================================"
echo "Benchmark: DuckDB lttb extension"
echo "================================================================================"
echo "DuckDB:    $DUCKDB (extension: $EXTENSION)"
if $HAS_CLICKHOUSE; then
    echo "ClickHouse: $CONTAINER (v$CH_VERSION)"
else
    echo "ClickHouse: not running (Part A skipped)"
fi
echo "Python:    lttb 0.3.2 + duckdb 1.4.5 + numpy 1.26.4"
echo "Date:      $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Method:    best of 5 runs"
echo "Notes:     DuckDB timing includes CLI subprocess startup."
echo "           ClickHouse timing is server-side (--time, excludes connection)."
echo "           Python timing is in-process (perf_counter, includes fetch+downsample)."
echo ""

# ============================================================================
# Part A: Three-way comparison (DuckDB vs ClickHouse vs Python)
# ============================================================================

if $HAS_CLICKHOUSE; then

# --- Section 1: Sorted DOUBLE input ---
echo "=== Section 1: Sorted DOUBLE input ==="
echo ""
printf "%-18s %6s %12s %12s %12s %12s\n" "Dataset" "n_out" "DuckDB(ms)" "ClickH(ms)" "Python(ms)" "DuckDB/CH"
printf "%-18s %6s %12s %12s %12s %12s\n" "................" "......" "............" "............" "............" "............"
echo "--------------------------------------------------------------------------------------------------------"

for SIZE in 10000 100000 1000000; do
    for N in 100 1000 10000; do
        [ "$N" -gt "$SIZE" ] && continue
        LABEL="$((SIZE / 1000))K"

        DDB_SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
        DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
        DDB=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

        CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT number::Float64 AS x, sin(number/${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
        CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
        CH=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

        PY=$(bench_python "$SIZE" "$N" "sorted")

        if [ "$CH" = "N/A" ]; then RATIO="N/A"
        else RATIO=$(echo "scale=2; $DDB / $CH" | bc -l); fi

        printf "%-18s %6s %12s %12s %12s %12s\n" "$LABEL" "$N" "$DDB" "$CH" "$PY" "${RATIO}x"
    done
done

# --- Section 2: Shuffled input ---
echo ""
echo "=== Section 2: Shuffled input (sort cost) ==="
echo ""
printf "%-18s %6s %12s %12s %12s\n" "Dataset" "n_out" "DuckDB(ms)" "ClickH(ms)" "Python(ms)*"
printf "%-18s %6s %12s %12s %12s\n" "................" "......" "............" "............" "............"
echo "------------------------------------------------------------------------------------------------"
echo "(* Python cannot handle shuffled input; uses sorted data for fair comparison)"

for SIZE in 10000 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K shuf"

    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i)) ORDER BY random();"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    DDB=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT x, y FROM (SELECT number::Float64 AS x, sin(number/${SIZE}.0)::Float64 AS y FROM numbers(${SIZE})) ORDER BY rand();"
    CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
    CH=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

    PY=$(bench_python "$SIZE" "$N" "sorted")

    printf "%-18s %6s %12s %12s %12s\n" "$LABEL" "$N" "$DDB" "$CH" "$PY"
done

# --- Section 3: TIMESTAMP input ---
echo ""
echo "=== Section 3: TIMESTAMP / DateTime input ==="
echo ""
printf "%-18s %6s %12s %12s %12s\n" "Dataset" "n_out" "DuckDB(ms)" "ClickH(ms)" "Python(ms)"
printf "%-18s %6s %12s %12s %12s\n" "................" "......" "............" "............" "............"
echo "------------------------------------------------------------------------------------------------"

for SIZE in 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K TS"

    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT to_timestamp(i) AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    DDB=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT toDateTime(number) AS x, sin(number/${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
    CH_QUERY="SELECT count() FROM (SELECT arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) FROM b)"
    CH=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

    # Python: fetch epoch microseconds as double (avoids pytz dependency)
    PY=$(python3 -c "
import duckdb, lttb, numpy as np, time
con = duckdb.connect()
size, n_out = $SIZE, $N
q = f'SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i) ORDER BY x'
best = None
for _ in range(6):
    s = time.perf_counter()
    data = con.execute(q).fetchall()
    arr = np.array(data, dtype=float)
    r = lttb.downsample(arr, n_out=n_out)
    e = time.perf_counter() - s
    if best is None or e < best: best = e
print(f'{best * 1000:.1f}')
con.close()
")

    printf "%-18s %6s %12s %12s %12s\n" "$LABEL" "$N" "$DDB" "$CH" "$PY"
done

# --- Section 4: Multi-group ---
echo ""
echo "=== Section 4: Multi-group (100 groups) ==="
echo ""
printf "%-18s %6s %12s %12s\n" "Dataset" "n_out" "DuckDB(ms)" "ClickH(ms)"
printf "%-18s %6s %12s %12s\n" "................" "......" "............" "............"
echo "------------------------------------------------------------------------------------------------"
echo "(Python lttb has no GROUP BY aggregate; excluded from this section)"

for SIZE in 100000 1000000; do
    N=100
    NG=100
    LABEL="$((SIZE / 1000))K ${NG}grp"

    DDB_SETUP="$LOAD CREATE TABLE b AS SELECT (i % ${NG})::INTEGER AS g, (i / ${NG})::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
    DDB_QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b GROUP BY g);"
    DDB=$(bench_duckdb "$DDB_SETUP" "$DDB_QUERY")

    CH_SETUP="DROP TABLE IF EXISTS b; CREATE TABLE b ENGINE=Memory AS SELECT (number % ${NG})::UInt32 AS g, (number / ${NG})::Float64 AS x, sin(number/${SIZE}.0)::Float64 AS y FROM numbers(${SIZE});"
    CH_QUERY="SELECT count() FROM (SELECT g, arrayJoin(largestTriangleThreeBuckets(${N})(x, y)) AS p FROM b GROUP BY g)"
    CH=$(bench_clickhouse "$CH_SETUP" "$CH_QUERY")

    printf "%-18s %6s %12s %12s\n" "$LABEL" "$N" "$DDB" "$CH"
done

echo ""

fi  # end Part A (HAS_CLICKHOUSE)

# ============================================================================
# Part B: DuckDB-internal function-level benchmark
# ============================================================================

# --- Section 5: All functions on sorted 1M input, n=1000 ---
echo "=== Section 5: All functions on sorted 1M input (n=1000) ==="
echo ""
printf "%-18s %12s\n" "Function" "Time(ms)"
printf "%-18s %12s\n" ".................." "............"
echo "--------------------------------------------------"

SETUP_SORTED="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/1000000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

for FUNC in lttb lttb_sorted lttb_indices; do
    QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(${FUNC}(x, y, 1000)) FROM b);"
    T=$(bench_duckdb "$SETUP_SORTED" "$QUERY")
    printf "%-18s %12s\n" "$FUNC" "$T"
done

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 1000, 4)) FROM b);"
T=$(bench_duckdb "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 1000, 8)) FROM b);"
T=$(bench_duckdb "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=8)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb_sorted(x, y, 1000, 4)) FROM b);"
T=$(bench_duckdb "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb_sorted(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, 1000)) FROM b);"
T=$(bench_duckdb "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "bucket_stats" "$T"

echo ""

# --- Section 6: lttb vs lttb_sorted (sort cost) at multiple scales ---
echo "=== Section 6: lttb vs lttb_sorted (sort cost) ==="
echo ""
printf "%-18s %6s %12s %12s %12s\n" "Dataset" "n_out" "lttb(ms)" "sorted(ms)" "Sort cost"
printf "%-18s %6s %12s %12s %12s\n" "................" "......" "............" "............" "............"
echo "------------------------------------------------------------------------"

for SIZE in 10000 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K"

    SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
    Q_LTTB="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    Q_SORTED="$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, ${N})) FROM b);"

    T_LTTB=$(bench_duckdb "$SETUP" "$Q_LTTB")
    T_SORTED=$(bench_duckdb "$SETUP" "$Q_SORTED")
    SORT_COST=$(echo "$T_LTTB - $T_SORTED" | bc -l | awk '{printf "%.1f", $1}')

    printf "%-18s %6s %12s %12s %12s\n" "$LABEL" "$N" "$T_LTTB" "$T_SORTED" "$SORT_COST"
done

echo ""

# --- Section 7: lttb vs minmax_lttb (preselect compute win) ---
echo "=== Section 7: lttb vs minmax_lttb (preselect compute win) ==="
echo ""
printf "%-18s %6s %12s %12s %12s\n" "Dataset" "n_out" "lttb(ms)" "minmax(ms)" "Speedup"
printf "%-18s %6s %12s %12s %12s\n" "................" "......" "............" "............" "............"
echo "------------------------------------------------------------------------"

for SIZE in 100000 1000000; do
    for N in 100 1000; do
        LABEL="$((SIZE / 1000))K"

        SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
        Q_LTTB="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
        Q_MINMAX="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, ${N}, 4)) FROM b);"

        T_LTTB=$(bench_duckdb "$SETUP" "$Q_LTTB")
        T_MINMAX=$(bench_duckdb "$SETUP" "$Q_MINMAX")

        if (( $(echo "$T_MINMAX > 0" | bc -l) )); then
            SPEEDUP=$(echo "scale=2; $T_LTTB / $T_MINMAX" | bc -l)
        else
            SPEEDUP="N/A"
        fi

        printf "%-18s %6s %12s %12s %12sx\n" "$LABEL" "$N" "$T_LTTB" "$T_MINMAX" "$SPEEDUP"
    done
done

echo ""
echo "--- shuffled input (bin-first sort elimination) ---"
echo ""
printf "%-18s %6s %12s %12s %12s\n" "Dataset" "n_out" "lttb(ms)" "minmax(ms)" "Speedup"
printf "%-18s %6s %12s %12s %12s\n" "................" "......" "............" "............" "............"
echo "------------------------------------------------------------------------"

for SIZE in 100000 1000000; do
    for N in 100 1000; do
        LABEL="$((SIZE / 1000))K shuf"

        SETUP="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i)) ORDER BY random();"
        Q_LTTB="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
        Q_MINMAX="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, ${N}, 4)) FROM b);"

        T_LTTB=$(bench_duckdb "$SETUP" "$Q_LTTB")
        T_MINMAX=$(bench_duckdb "$SETUP" "$Q_MINMAX")

        if (( $(echo "$T_MINMAX > 0" | bc -l) )); then
            SPEEDUP=$(echo "scale=2; $T_LTTB / $T_MINMAX" | bc -l)
        else
            SPEEDUP="N/A"
        fi

        printf "%-18s %6s %12s %12s %12sx\n" "$LABEL" "$N" "$T_LTTB" "$T_MINMAX" "$SPEEDUP"
    done
done

echo ""

# --- Section 8: bucket_stats at multiple scales ---
echo "=== Section 8: bucket_stats performance ==="
echo ""
printf "%-18s %6s %12s\n" "Dataset" "buckets" "Time(ms)"
printf "%-18s %6s %12s\n" "................" "......" "............"
echo "--------------------------------------------------"

for SIZE in 10000 100000 1000000; do
    for N in 100 1000; do
        LABEL="$((SIZE / 1000))K"

        SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
        QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, ${N})) FROM b);"

        T=$(bench_duckdb "$SETUP" "$QUERY")
        printf "%-18s %6s %12s\n" "$LABEL" "$N" "$T"
    done
done

echo ""

# --- Section 9: Shuffled input (lttb sort cost) ---
echo "=== Section 9: Shuffled input (sort cost) ==="
echo ""
printf "%-18s %6s %12s %12s %12s %12s\n" "Dataset" "n_out" "lttb(ms)" "sorted(ms)*" "minmax(ms)" "mm_sorted(ms)"
printf "%-18s %6s %12s %12s %12s %12s\n" "................" "......" "............" "............" "............" "............"
echo "--------------------------------------------------------------------------------------------------------"
echo "(* lttb_sorted on shuffled input gives WRONG results but shows sort-eliminated time)"
echo "(minmax_lttb and minmax_lttb_sorted use bin-first: correct results on shuffled input)"

for SIZE in 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K shuf"

    SETUP="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i)) ORDER BY random();"
    Q_LTTB="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    Q_SORTED="$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, ${N})) FROM b);"
    Q_MINMAX="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, ${N}, 4)) FROM b);"
    Q_MM_SORTED="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb_sorted(x, y, ${N}, 4)) FROM b);"

    T_LTTB=$(bench_duckdb "$SETUP" "$Q_LTTB")
    T_SORTED=$(bench_duckdb "$SETUP" "$Q_SORTED")
    T_MINMAX=$(bench_duckdb "$SETUP" "$Q_MINMAX")
    T_MM_SORTED=$(bench_duckdb "$SETUP" "$Q_MM_SORTED")

    printf "%-18s %6s %12s %12s %12s %12s\n" "$LABEL" "$N" "$T_LTTB" "$T_SORTED" "$T_MINMAX" "$T_MM_SORTED"
done

echo ""

# --- Section 10: Multi-group all functions ---
echo "=== Section 10: Multi-group (100 groups, 1M total) ==="
echo ""
printf "%-18s %12s\n" "Function" "Time(ms)"
printf "%-18s %12s\n" ".................." "............"
echo "--------------------------------------------------"

SETUP_GROUP="$LOAD CREATE TABLE b AS SELECT (i % 100)::INTEGER AS g, (i / 100)::DOUBLE AS x, sin(i/1000000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

for FUNC in lttb lttb_sorted lttb_indices; do
    QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(${FUNC}(x, y, 100)) FROM b GROUP BY g);"
    T=$(bench_duckdb "$SETUP_GROUP" "$QUERY")
    printf "%-18s %12s\n" "$FUNC" "$T"
done

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 100, 4)) FROM b GROUP BY g);"
T=$(bench_duckdb "$SETUP_GROUP" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb_sorted(x, y, 100, 4)) FROM b GROUP BY g);"
T=$(bench_duckdb "$SETUP_GROUP" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb_sorted(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, 100)) FROM b GROUP BY g);"
T=$(bench_duckdb "$SETUP_GROUP" "$QUERY")
printf "%-18s %12s\n" "bucket_stats" "$T"

echo ""
echo "================================================================================"
