#!/usr/bin/env python3
"""
Benchmark: DuckDB lttb extension vs Python lttb package.

Compares the performance of:
  1. DuckDB C++ lttb extension (native aggregate function)
  2. Python lttb package (fetch data → numpy → lttb.downsample)

Both process the same synthetic sine-wave datasets at 10K, 100K, and 1M
point scales, downsampling to n=100, 1000, and 10000 output points.

The DuckDB extension is invoked via the CLI binary (which matches the
extension's build version). Python lttb uses the pip-installed duckdb
package to generate and fetch data, then runs lttb.downsample in Python.

Usage:
  python3 scripts/benchmark_vs_python.py \\
    [--duckdb ./build/release/duckdb] \\
    [--extension ./build/release/extension/lttb/lttb.duckdb_extension]
"""

import argparse
import json
import os
import subprocess
import sys
import time

import duckdb
import lttb
import numpy as np


def run_duckdb_extension(duckdb_bin, extension_path, size, n_out, sorted_input=True):
    """Run the DuckDB lttb extension via CLI and return elapsed time in seconds."""
    if sorted_input:
        func = "lttb"
        table_sql = f"CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i);"
    else:
        func = "lttb"
        table_sql = (
            f"CREATE TABLE b AS SELECT x, y FROM "
            f"(SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i)) "
            f"ORDER BY random();"
        )

    sql = (
        ".timer on\n"
        f"LOAD '{extension_path}';\n"
        f"{table_sql}\n"
        f"SELECT count(*) FROM (SELECT unnest({func}(x, y, {n_out})) FROM b);\n"
    )

    # Run multiple times, take the best (exclude first run for cold start)
    times = []
    for _ in range(5):
        start = time.perf_counter()
        result = subprocess.run(
            [duckdb_bin, "-unsigned"],
            input=sql,
            capture_output=True,
            text=True,
            timeout=120,
        )
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            print(f"  DuckDB error: {result.stderr[:200]}", file=sys.stderr)
            return None
        times.append(elapsed)

    # Return min (best of 5) excluding the first (cold start)
    return min(times[1:]) if len(times) > 1 else times[0]


def run_duckdb_extension_sorted(duckdb_bin, extension_path, size, n_out):
    """Run lttb_sorted (skip sort) via CLI."""
    table_sql = f"CREATE TABLE b AS SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i);"
    sql = (
        ".timer on\n"
        f"LOAD '{extension_path}';\n"
        f"{table_sql}\n"
        f"SELECT count(*) FROM (SELECT unnest(lttb_sorted(x, y, {n_out})) FROM b);\n"
    )

    times = []
    for _ in range(5):
        start = time.perf_counter()
        result = subprocess.run(
            [duckdb_bin, "-unsigned"],
            input=sql,
            capture_output=True,
            text=True,
            timeout=120,
        )
        elapsed = time.perf_counter() - start
        if result.returncode != 0:
            print(f"  DuckDB error: {result.stderr[:200]}", file=sys.stderr)
            return None
        times.append(elapsed)

    return min(times[1:]) if len(times) > 1 else times[0]


def run_python_lttb(con, size, n_out, sorted_input=True):
    """Run Python lttb: fetch data from DuckDB, downsample in Python."""
    if sorted_input:
        query = f"SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i) ORDER BY x"
    else:
        query = f"SELECT x, y FROM (SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i)) ORDER BY random()"

    # Warm up + measure
    times = []
    for _ in range(5):
        start = time.perf_counter()
        data = con.execute(query).fetchall()
        arr = np.array(data, dtype=float)
        lttb.downsample(arr, n_out=n_out)
        elapsed = time.perf_counter() - start
        times.append(elapsed)

    return min(times[1:]) if len(times) > 1 else times[0]


def run_python_lttb_no_sort(con, size, n_out):
    """Run Python lttb without the ORDER BY (assumes data is already sorted by x)."""
    # range() generates sorted data, so no ORDER BY needed
    query = f"SELECT i::DOUBLE AS x, sin(i/{size}.0)::DOUBLE AS y FROM range({size}) AS t(i)"

    times = []
    for _ in range(5):
        start = time.perf_counter()
        data = con.execute(query).fetchall()
        arr = np.array(data, dtype=float)
        # lttb requires strictly increasing x; range() guarantees this
        lttb.downsample(arr, n_out=n_out)
        elapsed = time.perf_counter() - start
        times.append(elapsed)

    return min(times[1:]) if len(times) > 1 else times[0]


def format_time(seconds):
    if seconds is None:
        return "  N/A"
    if seconds < 0.001:
        return f"{seconds * 1e6:.0f}us"
    if seconds < 1.0:
        return f"{seconds * 1e3:.1f}ms"
    return f"{seconds:.2f}s"


def main():
    parser = argparse.ArgumentParser(description="Benchmark DuckDB lttb vs Python lttb")
    parser.add_argument("--duckdb", default="./build/release/duckdb", help="Path to duckdb CLI binary")
    parser.add_argument("--extension", default="./build/release/extension/lttb/lttb.duckdb_extension",
                        help="Path to lttb extension")
    parser.add_argument("--output", default=None, help="Output file for results (JSON)")
    args = parser.parse_args()

    duckdb_bin = args.duckdb
    extension_path = args.extension

    if not os.path.isfile(duckdb_bin):
        print(f"Error: duckdb binary not found at {duckdb_bin}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(extension_path):
        print(f"Error: extension not found at {extension_path}", file=sys.stderr)
        sys.exit(1)

    print("=" * 80)
    print("Benchmark: DuckDB lttb extension vs Python lttb package")
    print("=" * 80)
    print(f"DuckDB CLI:      {duckdb_bin}")
    print(f"Extension:        {extension_path}")
    print("Python lttb:      0.3.2")
    print(f"Python duckdb:    {duckdb.__version__}")
    print(f"NumPy:            {np.__version__}")
    print(f"Date:             {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}")
    print("Method:           best of 5 runs (excluding first cold start)")
    print()

    con = duckdb.connect()

    sizes = [10000, 100000, 1000000]
    n_outs = [100, 1000, 10000]

    results = []

    # Header
    print(f"{'Dataset':<25} {'n_out':>6} {'DuckDB lttb':>14} {'DuckDB sorted':>14} {'Python lttb':>14} {'Python no-sort':>16} {'Speedup':>8}")
    print(f"{'':.<25} {'':.<6} {'':.<14} {'':.<14} {'':.<14} {'':.<16} {'':.<8}")
    print("-" * 100)

    for size in sizes:
        for n_out in n_outs:
            # Skip n_out > size
            if n_out > size:
                continue

            label = f"{size // 1000}K sorted"

            # DuckDB extension (with sort)
            t_duckdb = run_duckdb_extension(duckdb_bin, extension_path, size, n_out, sorted_input=True)

            # DuckDB extension (skip sort)
            t_duckdb_sorted = run_duckdb_extension_sorted(duckdb_bin, extension_path, size, n_out)

            # Python lttb (with ORDER BY sort in SQL)
            t_python = run_python_lttb(con, size, n_out, sorted_input=True)

            # Python lttb (no sort, data already sorted from range())
            t_python_nosort = run_python_lttb_no_sort(con, size, n_out)

            # Speedup: DuckDB sorted vs Python no-sort (fairest: both skip sort)
            speedup = None
            if t_python_nosort and t_duckdb_sorted and t_duckdb_sorted > 0:
                speedup = t_python_nosort / t_duckdb_sorted
                speedup_str = f"{speedup:.1f}x"
            else:
                speedup_str = "N/A"

            print(f"{label:<25} {n_out:>6} {format_time(t_duckdb):>14} {format_time(t_duckdb_sorted):>14} "
                  f"{format_time(t_python):>14} {format_time(t_python_nosort):>16} {speedup_str:>8}")

            results.append({
                "size": size,
                "n_out": n_out,
                "sorted_input": True,
                "duckdb_lttb": t_duckdb,
                "duckdb_lttb_sorted": t_duckdb_sorted,
                "python_lttb": t_python,
                "python_lttb_nosort": t_python_nosort,
                "speedup_duckdb_sorted_vs_python_nosort": speedup if t_python_nosort and t_duckdb_sorted else None,
            })

    # Shuffled input comparison
    print()
    print(f"{'Dataset':<25} {'n_out':>6} {'DuckDB lttb':>14} {'DuckDB sorted':>14} {'Python lttb':>14} {'Python no-sort':>16} {'Speedup':>8}")
    print(f"{'':.<25} {'':.<6} {'':.<14} {'':.<14} {'':.<14} {'':.<16} {'':.<8}")
    print("-" * 100)

    for size in sizes:
        n_out = 1000
        label = f"{size // 1000}K shuffled"

        # DuckDB extension on shuffled input (sorts internally)
        t_duckdb = run_duckdb_extension(duckdb_bin, extension_path, size, n_out, sorted_input=False)

        # DuckDB extension sorted on shuffled (skips sort, wrong result but measures skip)
        # Use the sorted table for lttb_sorted (fair: both process sorted data)
        t_duckdb_sorted = run_duckdb_extension_sorted(duckdb_bin, extension_path, size, n_out)

        # Python lttb on shuffled (fetches shuffled data, lttb requires sorted so this would fail
        # or give wrong results — but lttb actually doesn't sort, it validates strictly increasing)
        # For shuffled data, Python lttb would raise an error. So we compare:
        # - DuckDB lttb on shuffled (sorts internally) vs Python lttb on sorted (fair: both produce correct result)
        t_python = run_python_lttb(con, size, n_out, sorted_input=True)  # sorted, for fair comparison
        t_python_nosort = run_python_lttb_no_sort(con, size, n_out)

        speedup = None
        if t_python_nosort and t_duckdb and t_duckdb > 0:
            speedup = t_python_nosort / t_duckdb
            speedup_str = f"{speedup:.1f}x"
        else:
            speedup_str = "N/A"

        print(f"{label:<25} {n_out:>6} {format_time(t_duckdb):>14} {format_time(t_duckdb_sorted):>14} "
              f"{format_time(t_python):>14} {format_time(t_python_nosort):>16} {speedup_str:>8}")

        results.append({
            "size": size,
            "n_out": n_out,
            "sorted_input": False,
            "duckdb_lttb_shuffled": t_duckdb,
            "duckdb_lttb_sorted": t_duckdb_sorted,
            "python_lttb_sorted": t_python,
            "python_lttb_nosort": t_python_nosort,
            "speedup_duckdb_shuffled_vs_python_nosort": speedup if t_python_nosort and t_duckdb else None,
        })

    # Summary
    print()
    print("=" * 80)
    print("Summary")
    print("=" * 80)
    print()
    print("The 'Speedup' column compares DuckDB lttb_sorted (C++ native, no sort)")
    print("against Python lttb with no ORDER BY (Python native, no sort).")
    print("This is the fairest apples-to-apples comparison of the core")
    print("downsampling algorithm: C++ vs Python, both on pre-sorted data.")
    print()
    print("For shuffled input, 'Speedup' compares DuckDB lttb (sorts internally)")
    print("against Python lttb on pre-sorted data (Python can't handle shuffled).")
    print("This shows the cost of DuckDB's internal sort vs Python's requirement")
    print("for pre-sorted input.")
    print()

    if args.output:
        with open(args.output, "w") as f:
            json.dump({
                "date": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                "duckdb_version": duckdb.__version__,
                "python_lttb_version": "0.3.2",
                "numpy_version": np.__version__,
                "results": results,
            }, f, indent=2)
        print(f"Results saved to {args.output}")

    con.close()


if __name__ == "__main__":
    main()
