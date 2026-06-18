#!/usr/bin/env bash
# Benchmark script for duckdb-lttb extension.
#
# Measures LTTB downsampling performance at 100K and 1M point scales,
# tracking sort, sampling, and finalize costs across:
#   - lttb vs lttb_sorted vs lttb_indices
#   - different output sizes (n = 100, 1000, 10000)
#   - DOUBLE vs TIMESTAMP input types
#   - single-series vs multi-group workloads
#   - sorted vs shuffled input (sort cost isolation)
#
# Usage:
#   ./scripts/benchmark_lttb.sh [path/to/duckdb] [path/to/lttb.duckdb_extension]
#
# Defaults:
#   duckdb:   ./build/release/duckdb
#   extension: ./build/release/extension/lttb/lttb.duckdb_extension

set -euo pipefail

DUCKDB="${1:-./build/release/duckdb}"
EXTENSION="${2:-./build/release/extension/lttb/lttb.duckdb_extension}"

if [ ! -x "$DUCKDB" ]; then
    echo "Error: duckdb binary not found at $DUCKDB" >&2
    exit 1
fi
if [ ! -f "$EXTENSION" ]; then
    echo "Error: extension not found at $EXTENSION" >&2
    exit 1
fi

# Run a full SQL script (setup + query) in a single DuckDB session with
# .timer on. The setup runs first (table creation), then the timed query.
# $1 = label, $2 = setup SQL, $3 = query SQL
run_bench() {
    local label="$1"
    local setup="$2"
    local query="$3"
    echo ""
    echo "--- $label ---"
    printf ".timer on\n%s\n%s\n" "$setup" "$query" | "$DUCKDB" -unsigned 2>&1 \
        | grep -E "Run Time|count_star" | tail -2
}

echo "duckdb-lttb Benchmark"
echo "======================"
echo "Binary:    $DUCKDB"
echo "Extension: $EXTENSION"
echo "Date:      $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""

LOAD="LOAD '$EXTENSION';"

# ---------------------------------------------------------------------------
# Section 1: 100K points — lttb vs lttb_sorted vs lttb_indices
# ---------------------------------------------------------------------------

echo "=== Section 1: 100K DOUBLE points ==="

SETUP_100K="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/100.0)::DOUBLE AS y FROM range(100000) AS t(i);"

for N in 100 1000 10000; do
    run_bench "100K → n=$N | lttb"        "$SETUP_100K" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, $N)) FROM b);"
    run_bench "100K → n=$N | lttb_sorted" "$SETUP_100K" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, $N)) FROM b);"
    run_bench "100K → n=$N | lttb_indices" "$SETUP_100K" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_indices(x, y, $N)) FROM b);"
done

# ---------------------------------------------------------------------------
# Section 2: 1M points — lttb vs lttb_sorted vs lttb_indices
# ---------------------------------------------------------------------------

echo ""
echo "=== Section 2: 1M DOUBLE points ==="

SETUP_1M="$LOAD CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/1000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

for N in 100 1000 10000; do
    run_bench "1M → n=$N | lttb"         "$SETUP_1M" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, $N)) FROM b);"
    run_bench "1M → n=$N | lttb_sorted"  "$SETUP_1M" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, $N)) FROM b);"
    run_bench "1M → n=$N | lttb_indices" "$SETUP_1M" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_indices(x, y, $N)) FROM b);"
done

# ---------------------------------------------------------------------------
# Section 3: TIMESTAMP input (1M points) — type conversion overhead
# ---------------------------------------------------------------------------

echo ""
echo "=== Section 3: 1M TIMESTAMP points (type conversion overhead) ==="

SETUP_TS="$LOAD CREATE TABLE b AS SELECT to_timestamp(i) AS x, sin(i/1000.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

run_bench "1M TIMESTAMP → n=1000 | lttb"        "$SETUP_TS" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, 1000)) FROM b);"
run_bench "1M TIMESTAMP → n=1000 | lttb_sorted" "$SETUP_TS" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, 1000)) FROM b);"

# ---------------------------------------------------------------------------
# Section 4: Multi-group (100 groups × 10K points each = 1M total)
# ---------------------------------------------------------------------------

echo ""
echo "=== Section 4: 1M multi-group (100 groups × 10K each) ==="

SETUP_GROUPS="$LOAD CREATE TABLE b AS SELECT (i % 100)::INTEGER AS g, (i / 100)::DOUBLE AS x, sin(i/100.0)::DOUBLE AS y FROM range(1000000) AS t(i);"

run_bench "1M 100 groups → n=100/group | lttb"        "$SETUP_GROUPS" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, 100)) FROM b GROUP BY g);"
run_bench "1M 100 groups → n=100/group | lttb_sorted" "$SETUP_GROUPS" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, 100)) FROM b GROUP BY g);"

# ---------------------------------------------------------------------------
# Section 5: Shuffled input — sort cost isolation
# ---------------------------------------------------------------------------

echo ""
echo "=== Section 5: 1M shuffled input (sort cost isolation) ==="

SETUP_SHUFFLE="$LOAD CREATE TABLE b AS SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/1000.0)::DOUBLE AS y FROM range(1000000) AS t(i)) ORDER BY random();"

run_bench "1M shuffled → n=1000 | lttb (sorts internally)"        "$SETUP_SHUFFLE" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb(x, y, 1000)) FROM b);"
run_bench "1M shuffled → n=1000 | lttb_sorted (skips sort)"       "$SETUP_SHUFFLE" "$LOAD SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, 1000)) FROM b);"

# ---------------------------------------------------------------------------
# Section 6: Results interpretation
# ---------------------------------------------------------------------------

echo ""
echo "=== Section 6: Results interpretation ==="
echo ""
echo "The benchmark results above allow isolating the following costs:"
echo ""
echo "  Sort cost:       lttb(sorted) - lttb_sorted(sorted)"
echo "                   or lttb(shuffled) - lttb_sorted(shuffled)"
echo "  Sampling+final:  lttb_sorted time (no sort overhead)"
echo "  Type conversion: lttb(TIMESTAMP) - lttb(DOUBLE)"
echo "  Combine overhead: multi-group lttb - single-group lttb"
echo ""
echo "Note: DuckDB's PRAGMA profiling shows operator-level timing"
echo "(SEQ_SCAN, HASH_GROUP_BY, etc.) but does not break down the internal"
echo "steps of an aggregate function (sort, sampling, finalize). The"
echo "shuffled-vs-sorted comparison above is the primary way to isolate"
echo "the sort cost."
echo ""
echo "For deeper profiling, use PRAGMA enable_profiling=json and inspect"
echo "the full JSON output, or add timing instrumentation inside the"
echo "extension's Downsample/Update/Finalize functions."
echo ""
echo "======================"
echo "Benchmark complete."
