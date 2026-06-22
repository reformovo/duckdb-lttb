#!/usr/bin/env bash
# DuckDB lttb extension: function-level benchmark.
#
# Benchmarks all registered functions on the same synthetic sine-wave datasets:
#   - lttb            (sort + LTTB)
#   - lttb_sorted     (skip sort, caller guarantees sorted input)
#   - lttb_indices    (sort + LTTB, index output)
#   - minmax_lttb     (MinMax preselect + LTTB)
#   - bucket_stats    (per-bucket statistics)
#
# Also compares lttb vs lttb_sorted (sort cost) and lttb vs minmax_lttb
# (preselect compute win).
#
# Usage:
#   ./scripts/benchmark_functions.sh \
#     [--duckdb ./build/release/duckdb] \
#     [--extension ./build/release/extension/lttb/lttb.duckdb_extension]

set -euo pipefail

DUCKDB="./build/release/duckdb"
EXTENSION="./build/release/extension/lttb/lttb.duckdb_extension"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duckdb)    DUCKDB="$2"; shift 2 ;;
        --extension) EXTENSION="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -x "$DUCKDB" ]; then echo "Error: duckdb not found at $DUCKDB" >&2; exit 1; fi
if [ ! -f "$EXTENSION" ]; then echo "Error: extension not found at $EXTENSION" >&2; exit 1; fi

LOAD="LOAD '$EXTENSION';"

# --- Timing helper: best of N, returns ms (includes CLI startup + query) ---
bench() {
    local setup="$1" query="$2" runs="${3:-5}" best="" t
    for i in $(seq 1 "$runs"); do
        t=$(printf ".timer on\n%s\n%s\n" "$setup" "$query" | "$DUCKDB" -unsigned 2>&1 \
            | grep "Run Time" | tail -1 | sed 's/.*real \([0-9.]*\).*/\1/')
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best="$t"; fi
    done
    echo "$best" | awk '{printf "%.1f", $1 * 1000}'
}

# --- Output header ---
echo "================================================================================"
echo "DuckDB lttb Extension: Function-Level Benchmark"
echo "================================================================================"
echo "DuckDB:   $DUCKDB"
echo "Extension: $EXTENSION"
echo "Date:     $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Method:   best of 5 runs (CLI .timer, includes subprocess startup ~1ms)"
echo "Data:     synthetic sine wave sin(i/scale), DOUBLE type"
echo ""

# --- Section 1: All functions on sorted 1M input, n=1000 ---
echo "=== Section 1: All functions on sorted 1M input (n=1000) ==="
echo ""
printf "%-18s %12s\n" "Function" "Time(ms)"
printf "%-18s %12s\n" ".................." "............"
echo "--------------------------------------------------"

SETUP_SORTED="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/1000000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

for FUNC in lttb lttb_sorted lttb_indices; do
    QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(${FUNC}(x, y, 1000)) FROM b);"
    T=$(bench "$SETUP_SORTED" "$QUERY")
    printf "%-18s %12s\n" "$FUNC" "$T"
done

# minmax_lttb: 4th arg minmax_ratio
QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 1000, 4)) FROM b);"
T=$(bench "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 1000, 8)) FROM b);"
T=$(bench "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=8)" "$T"

# bucket_stats
QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, 1000)) FROM b);"
T=$(bench "$SETUP_SORTED" "$QUERY")
printf "%-18s %12s\n" "bucket_stats" "$T"

echo ""

# --- Section 2: lttb vs lttb_sorted (sort cost) at multiple scales ---
echo "=== Section 2: lttb vs lttb_sorted (sort cost) ==="
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

    T_LTTB=$(bench "$SETUP" "$Q_LTTB")
    T_SORTED=$(bench "$SETUP" "$Q_SORTED")
    SORT_COST=$(echo "$T_LTTB - $T_SORTED" | bc -l | awk '{printf "%.1f", $1}')

    printf "%-18s %6s %12s %12s %12s\n" "$LABEL" "$N" "$T_LTTB" "$T_SORTED" "$SORT_COST"
done

echo ""

# --- Section 3: lttb vs minmax_lttb (preselect compute win) ---
echo "=== Section 3: lttb vs minmax_lttb (preselect compute win) ==="
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

        T_LTTB=$(bench "$SETUP" "$Q_LTTB")
        T_MINMAX=$(bench "$SETUP" "$Q_MINMAX")

        if (( $(echo "$T_MINMAX > 0" | bc -l) )); then
            SPEEDUP=$(echo "scale=2; $T_LTTB / $T_MINMAX" | bc -l)
        else
            SPEEDUP="N/A"
        fi

        printf "%-18s %6s %12s %12s %12sx\n" "$LABEL" "$N" "$T_LTTB" "$T_MINMAX" "$SPEEDUP"
    done
done

echo ""

# --- Section 4: bucket_stats at multiple scales ---
echo "=== Section 4: bucket_stats performance ==="
echo ""
printf "%-18s %6s %12s\n" "Dataset" "buckets" "Time(ms)"
printf "%-18s %6s %12s\n" "................" "......" "............"
echo "--------------------------------------------------"

for SIZE in 10000 100000 1000000; do
    for N in 100 1000; do
        LABEL="$((SIZE / 1000))K"

        SETUP="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i);"
        QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, ${N})) FROM b);"

        T=$(bench "$SETUP" "$QUERY")
        printf "%-18s %6s %12s\n" "$LABEL" "$N" "$T"
    done
done

echo ""

# --- Section 5: Shuffled input (lttb sort cost vs lttb_sorted misuse) ---
echo "=== Section 5: Shuffled input (lttb sort cost) ==="
echo ""
printf "%-18s %6s %12s %12s\n" "Dataset" "n_out" "lttb(ms)" "sorted(ms)*"
printf "%-18s %6s %12s %12s\n" "................" "......" "............" "............"
echo "----------------------------------------------------------------"
echo "(* lttb_sorted on shuffled input gives WRONG results but shows sort-eliminated time)"

for SIZE in 100000 1000000; do
    N=1000
    LABEL="$((SIZE / 1000))K shuf"

    SETUP="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/${SIZE}.0)::DOUBLE AS y FROM range(${SIZE}) AS t(i)) ORDER BY random();"
    Q_LTTB="$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, ${N})) FROM b);"
    Q_SORTED="$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, ${N})) FROM b);"

    T_LTTB=$(bench "$SETUP" "$Q_LTTB")
    T_SORTED=$(bench "$SETUP" "$Q_SORTED")

    printf "%-18s %6s %12s %12s\n" "$LABEL" "$N" "$T_LTTB" "$T_SORTED"
done

echo ""

# --- Section 6: Multi-group (100 groups) all functions ---
echo "=== Section 6: Multi-group (100 groups, 1M total) ==="
echo ""
printf "%-18s %12s\n" "Function" "Time(ms)"
printf "%-18s %12s\n" ".................." "............"
echo "--------------------------------------------------"

SETUP_GROUP="$LOAD CREATE TABLE b AS SELECT (i % 100)::INTEGER AS g, (i / 100)::DOUBLE AS x, sin(i/1000000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

for FUNC in lttb lttb_sorted lttb_indices; do
    QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(${FUNC}(x, y, 100)) FROM b GROUP BY g);"
    T=$(bench "$SETUP_GROUP" "$QUERY")
    printf "%-18s %12s\n" "$FUNC" "$T"
done

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(minmax_lttb(x, y, 100, 4)) FROM b GROUP BY g);"
T=$(bench "$SETUP_GROUP" "$QUERY")
printf "%-18s %12s\n" "minmax_lttb(r=4)" "$T"

QUERY="$LOAD SELECT count(*) FROM (SELECT unnest(bucket_stats(x, y, 100)) FROM b GROUP BY g);"
T=$(bench "$SETUP_GROUP" "$QUERY")
printf "%-18s %12s\n" "bucket_stats" "$T"

echo ""
echo "================================================================================"
