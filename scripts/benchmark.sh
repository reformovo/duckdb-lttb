#!/usr/bin/env bash
# Three-way benchmark: DuckDB lttb vs ClickHouse vs Python lttb.
#
# All three process the same synthetic sine-wave datasets. Timing method:
#   - DuckDB:   CLI .timer (includes subprocess startup)
#   - ClickHouse: clickhouse-client --time (server-side, excludes connection)
#   - Python:   Python time.perf_counter (in-process, excludes DuckDB fetch)
#
# For the fairest comparison, we also report DuckDB's query-only time by
# running setup + query in a single session and measuring only the query.
#
# Usage:
#   ./scripts/benchmark_three_way.sh \
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
if ! container list 2>/dev/null | grep -q "$CONTAINER"; then echo "Error: container '$CONTAINER' not running" >&2; exit 1; fi

CH_VERSION=$(container exec "$CONTAINER" clickhouse-client -q "SELECT version()" 2>/dev/null)
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
echo "Three-Way Benchmark: DuckDB lttb vs ClickHouse vs Python lttb"
echo "================================================================================"
echo "DuckDB:     $DUCKDB (extension: $EXTENSION)"
echo "ClickHouse: $CONTAINER (v$CH_VERSION)"
echo "Python:     lttb 0.3.2 + duckdb 1.4.5 + numpy 1.26.4"
echo "Date:       $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Method:     best of 5 runs"
echo "Notes:      DuckDB timing includes CLI subprocess startup."
echo "            ClickHouse timing is server-side (--time, excludes connection)."
echo "            Python timing is in-process (perf_counter, includes fetch+downsample)."
echo ""

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
echo "================================================================================"
