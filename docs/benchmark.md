# LTTB Performance: DuckDB vs ClickHouse vs Python

## Test Environment

| Item | Version / Configuration |
|---|---|
| DuckDB | v1.5.3, local Release build, ninja + ccache |
| DuckDB lttb extension | This project, native C++ aggregate functions |
| ClickHouse | v25.7.1.3997, `clickhouse-server:latest-alpine` |
| Python lttb | 0.3.2, installed with pip, NumPy backend |
| Python duckdb | 1.4.5, installed with pip |
| NumPy | 1.26.4 |
| Runtime environment | macOS arm64, Apple Silicon |
| ClickHouse runtime | Apple Container, Linux/arm64 virtualization framework |
| DuckDB runtime | Native macOS arm64 binary |
| Method | Best of 5 runs |
| Dataset | Synthetic sine wave, `sin(i/scale)`, DOUBLE values |

### Timing Method

| Implementation | Timing Source | Included Overhead |
|---|---|---|
| DuckDB | CLI `.timer on` | Subprocess startup + SQL parsing + query execution |
| ClickHouse | `clickhouse-client --time` | Server-side query execution, excluding connection and transfer |
| Python | `time.perf_counter` | Data fetch (`fetchall`) + NumPy conversion + `lttb.downsample` |

> **Note**: DuckDB timings include CLI subprocess startup overhead, about 1 ms. ClickHouse timings are server-side query execution times only. Therefore, DuckDB's actual query execution time is lower than the table values. Python timings include the `fetchall` overhead from DuckDB, which is an inherent cost of a Python UDF-style workflow.

---

## 1. Sorted DOUBLE Input

The following table compares downsampling the same sorted sine-wave dataset with all three implementations:

| Input Size | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms) | DuckDB/CH |
|---|---|---|---|---|---|
| 10K | 100 | 1.0 | 2.0 | 3.1 | 0.50x |
| 10K | 1000 | 1.0 | 2.0 | 9.8 | 0.50x |
| 10K | 10000 | 1.0 | 2.0 | 2.2 | 0.50x |
| 100K | 100 | 2.0 | 3.0 | 25.6 | 0.66x |
| 100K | 1000 | 2.0 | 3.0 | 31.5 | 0.66x |
| 100K | 10000 | 2.0 | 3.0 | 95.6 | 0.66x |
| 1M | 100 | 17.0 | 19.0 | 239.9 | 0.89x |
| 1M | 1000 | 16.0 | 18.0 | 247.9 | 0.88x |
| 1M | 10000 | 16.0 | 19.0 | 311.8 | 0.84x |

### Analysis

- **DuckDB is fastest at every tested scale**, followed by ClickHouse, with Python slowest.
- **10K-100K**: DuckDB is 1.5-2x faster than ClickHouse and 3-48x faster than Python. DuckDB's roughly 1 ms CLI startup overhead is a meaningful share at this size, but DuckDB still leads.
- **1M**: DuckDB and ClickHouse are close, 16 ms vs 18 ms, a 0.88x ratio. Both are native C++ implementations, so the core algorithm cost is similar. Python takes 248 ms, about 15x slower.
- **Python sensitivity to n_out**: Python slows down substantially at `n_out=10000`, for example 96 ms on 100K rows vs 26 ms at `n_out=100`, because `np.array_split` and the per-bucket loop scale with the number of buckets. DuckDB and ClickHouse are much less sensitive to `n_out`.

---

## 2. Shuffled Input and Sorting Cost

This test downsamples randomly shuffled input and measures the cost of internal sorting:

| Input Size | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms)* |
|---|---|---|---|---|
| 10K shuffled | 1000 | 1.0 | 2.0 | 9.9 |
| 100K shuffled | 1000 | 6.0 | 8.0 | 31.6 |
| 1M shuffled | 1000 | 74.0 | 85.0 | 244.5 |

> *The Python `lttb` package requires strictly increasing input and cannot process shuffled data. The Python value uses sorted input and is included only as a reference baseline.

### Analysis

- **DuckDB remains faster than ClickHouse on shuffled input**: 74 ms vs 85 ms on 1M shuffled rows, a 0.87x ratio.
- **Sorting cost**, compared with sorted input:
  - DuckDB: 1M sorting cost = 74 ms - 16 ms = **58 ms**, 78% of total time.
  - ClickHouse: 1M sorting cost = 85 ms - 18 ms = **67 ms**, 79% of total time.
  - The two sorting costs are close, with DuckDB slightly ahead.
- **Python cannot process shuffled input**, which is a key advantage of database aggregate implementations: users do not have to pre-sort their data outside SQL.

---

## 3. TIMESTAMP / DateTime Input

This test measures the conversion overhead for timestamp-like input:

| Input Size | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms) |
|---|---|---|---|---|
| 100K TIMESTAMP | 1000 | 2.0 | 3.0 | 31.7 |
| 1M TIMESTAMP | 1000 | 16.0 | 17.0 | 254.7 |

### Analysis

- **Type conversion overhead is negligible**: DuckDB and ClickHouse have almost identical timings for TIMESTAMP/DateTime and DOUBLE input, for example 2 ms vs 2 ms and 16 ms vs 16 ms. Both convert internally to epoch-style double values for computation.
- **Python** TIMESTAMP timing is close to DOUBLE timing, 254 ms vs 248 ms, because the Python version receives timestamps as doubles and is dominated by `fetchall`.
- **DuckDB preserves output types**: `lttb(x, y, n)` returns `STRUCT(x TIMESTAMP, y DOUBLE)[]` when `x` is TIMESTAMP, so users do not have to reconstruct timestamps manually with `to_timestamp()`. ClickHouse also preserves the x type, and Python preserves NumPy dtypes.

---

## 4. Multi-Group GROUP BY

This test measures the combine path while downsampling 100 groups in parallel, with 1K-10K points per group:

| Input Size | n_out/group | DuckDB (ms) | ClickHouse (ms) |
|---|---|---|---|
| 100K (100 groups) | 100 | 2.0 | 4.0 |
| 1M (100 groups) | 100 | 12.0 | 21.0 |

> The Python `lttb` package is a pure function and does not support SQL GROUP BY aggregation, so it is not included here.

### Analysis

- **DuckDB has a clear advantage in multi-group workloads**: 12 ms vs 21 ms on 1M rows / 100 groups, **1.75x** faster.
- DuckDB's combine optimization transfers the source vector pointer in O(1) when the target group is empty, reducing memory copies in grouped execution.
- With 100K rows / 100 groups, each group has only 1000 points. DuckDB takes 2 ms vs ClickHouse's 4 ms, about 2x faster.

---

## 5. Summary Matrix

| Dimension | DuckDB lttb | ClickHouse | Python lttb |
|---|---|---|---|
| **10K sorted** | **1.0 ms** | 2.0 ms | 3.1 ms |
| **100K sorted** | **2.0 ms** | 3.0 ms | 25.6 ms |
| **1M sorted** | **16.0 ms** | 18.0 ms | 248 ms |
| **1M shuffled** | **74.0 ms** | 85.0 ms | N/A, unsupported |
| **1M TIMESTAMP** | **16.0 ms** | 17.0 ms | 255 ms |
| **1M / 100 groups** | **12.0 ms** | 21.0 ms | N/A, unsupported |
| **Internal sorting** | Yes, `stable_sort` | Yes, unstable | No, requires pre-sorted input |
| **Type preservation** | Yes | Yes | Yes, NumPy dtype |
| **GROUP BY** | Yes | Yes | No |
| **Shuffled input** | Supported | Supported | Unsupported |
| **Index output** | `lttb_indices` | None | None |
| **Sorted fast path** | `lttb_sorted` | None | N/A |
| **Implementation language** | C++ | C++ | Python, NumPy |

---

## 6. Conclusions

### Performance Ranking

1. **DuckDB lttb extension**: fastest in every tested scenario.
2. **ClickHouse largestTriangleThreeBuckets**: close behind, nearly tied at 1M rows.
3. **Python lttb**: significantly slower and more limited.

### Key Findings

- **DuckDB vs ClickHouse**: Both are native C++ implementations and have similar core algorithm performance at 1M points, 16 ms vs 18 ms. DuckDB's advantage comes mainly from lower process overhead in this setup, native macOS vs Linux container, grouped combine optimization, and lower sensitivity to `n_out`.
- **C++ vs Python**: At 1M points, the C++ implementations are about **15x** faster than Python. Python is bottlenecked by `fetchall` serialization, NumPy array conversion, and the per-bucket Python loop.
- **Database aggregate vs external function**: DuckDB and ClickHouse can apply LTTB directly inside SQL with GROUP BY, WHERE filters, JOINs, and other relational operations. Python `lttb` requires pre-sorted input and does not support GROUP BY, so real workflows need additional data movement.
- **Sorting cost**: For shuffled input, sorting accounts for about 78% of total runtime, 58 ms / 74 ms in DuckDB and 67 ms / 85 ms in ClickHouse. The `lttb_sorted` fast path removes this cost entirely when the caller can guarantee sorted input, for example on a table ordered by a time-series primary key.

### Environment Notes

- ClickHouse ran inside a Linux container using Apple virtualization, while DuckDB ran natively on macOS. This affects 10K-100K timings through startup and connection overhead, but has less impact on 1M-scale core algorithm cost, where the two implementations are close.
- DuckDB timings include CLI subprocess startup, about 1 ms. ClickHouse timings are server-side query time only. DuckDB's actual query execution time is therefore lower than the table values.
- Python timings include `fetchall` from DuckDB, which is an inherent cost of the Python UDF-style workflow and cannot be avoided in that setup.

---

## 7. Per-Function Performance Inside DuckDB

This section compares all registered functions from the DuckDB lttb extension on the same datasets and environment. It is generated by Part B, Sections 5-10, of `scripts/benchmark.sh`. It requires only DuckDB and the extension, not ClickHouse or Python.

### 7.1 All Functions, 1M Sorted Input, n=1000

| Function | Time (ms) | Notes |
|---|---|---|
| `lttb` | 17.0 | Sort + LTTB sampling |
| `lttb_sorted` | 10.0 | Skip sorting, direct LTTB sampling |
| `lttb_indices` | 17.0 | Sort + LTTB, output selected indices |
| `minmax_lttb(r=4)` | 11.0 | Bin-first preselection + LTTB, skipping full sort |
| `minmax_lttb(r=8)` | 10.0 | Bin-first preselection with a larger candidate set + LTTB |
| `minmax_lttb_sorted(r=4)` | 11.0 | Same as `minmax_lttb`, sorted-input variant |
| `bucket_stats` | 16.0 | Equal-count bucketing + statistical aggregation |

### Analysis

- **`lttb_sorted` is the fastest downsampling function**: 10 ms vs 17 ms for `lttb`. The 7 ms sorting cost is 41% of total time. Use it when input ordering by `x` is already guaranteed.
- **`minmax_lttb` now has a clear speedup**: 11 ms vs 17 ms for `lttb`, a **1.55x** improvement. The Phase 1 optimization replaced "full sort, then preselect" with a bin-first strategy: scan once to find the x range, assign all points to equal-width x-bins, keep each bin's argmin/argmax, sort only about `n * ratio` candidates, then run LTTB. This removes the full `stable_sort` that previously dominated runtime. Increasing `minmax_ratio` from 4 to 8 creates a larger candidate set, but LTTB remains fast because candidate sorting is negligible.
- **`bucket_stats` is similar to `lttb`**: 16 ms vs 17 ms. It avoids triangle-area computation, but still performs sorting plus per-bucket min/max/mean/std aggregation.
- **`lttb_indices` matches `lttb`**: the extra index output has negligible overhead.

### 7.2 lttb vs lttb_sorted, Sorting Cost

| Input Size | n_out | lttb (ms) | lttb_sorted (ms) | Sorting Cost (ms) |
|---|---|---|---|---|
| 10K | 1000 | 0.0 | 1.0 | ~0 |
| 100K | 1000 | 2.0 | 1.0 | 1.0 |
| 1M | 1000 | 15.0 | 9.0 | 6.0 |

### Analysis

- Sorting cost grows with input size: about 1 ms at 100K and 6 ms at 1M.
- On sorted input, `lttb_sorted` provides a **1.67x speedup** at 1M rows.
- At 10K rows, CLI startup overhead of about 1 ms dominates the timing, so sorting cost cannot be measured reliably.

### 7.3 lttb vs minmax_lttb, Preselection Benefit and Overhead

| Input Size | n_out | lttb (ms) | minmax_lttb (ms) | Speedup |
|---|---|---|---|---|
| 100K | 100 | 2.0 | 1.0 | 2.00x |
| 100K | 1000 | 2.0 | 1.0 | 2.00x |
| 1M | 100 | 16.0 | 11.0 | 1.45x |
| 1M | 1000 | 17.0 | 11.0 | 1.55x |

### Analysis

- **`minmax_lttb` now has a stable speedup**: 1.55x at 1M/n=1000 and 1.45x at 1M/n=100. The Phase 1 bin-first optimization removes the full sort and replaces O(n log n) sorting with two O(n) scans, one to find the x range and one to build bins.
- **The shuffled-input speedup is larger**: see Section 7.5. A 1M shuffled dataset improves from 75 ms to 10 ms, a **7.5x speedup**, because sorting accounts for 88% of the `lttb` runtime on shuffled input.
- **Conclusion**: `minmax_lttb` is now the recommended function for large unsorted datasets. For sorted input, `lttb_sorted` at 10 ms and `minmax_lttb` at 11 ms are close, with `lttb_sorted` slightly ahead because it does not need the binning scan.

### 7.4 bucket_stats Performance

| Input Size | Buckets | Time (ms) |
|---|---|---|
| 10K | 100 | 1.0 |
| 10K | 1000 | 1.0 |
| 100K | 100 | 2.0 |
| 100K | 1000 | 2.0 |
| 1M | 100 | 16.0 |
| 1M | 1000 | 15.0 |

### Analysis

- `bucket_stats` performance is similar to `lttb`, 15 ms vs 15 ms at 1M rows. It avoids triangle-area computation, but sorting plus per-bucket aggregation of min/max/mean/std/first/last has similar cost.
- The number of buckets has no significant impact on performance because statistical aggregation is an O(n) linear scan, independent of bucket count.

### 7.5 Shuffled Input, Sorting Cost Validation

| Input Size | n_out | lttb (ms) | lttb_sorted (ms)* | minmax_lttb (ms) | minmax_lttb_sorted (ms) |
|---|---|---|---|---|---|
| 100K shuffled | 1000 | 6.0 | 1.0 | 2.0 | 1.0 |
| 1M shuffled | 1000 | 75.0 | 9.0 | 10.0 | 13.0 |

> *`lttb_sorted` returns incorrect results on shuffled input. It is shown here only to isolate the cost of sorting.

### Analysis

- Sorting cost is substantial on shuffled input: at 1M rows, 75 ms - 9 ms = **66 ms**, 88% of total time.
- Compared with the 7 ms sorting cost on sorted input, shuffled-input sorting is much more expensive because `stable_sort` reaches its worse O(n log^2 n) behavior on unordered data.
- **`minmax_lttb` achieves a 7.5x speedup on shuffled input**: 1M shuffled rows improve from 75 ms with `lttb` to 10 ms with `minmax_lttb`. The bin-first strategy completely skips the full sort and uses two O(n) scans instead. `minmax_lttb_sorted`, 13 ms, is slightly slower than `minmax_lttb`, 10 ms, but both follow the same bin-first path and the difference is within measurement noise.
- **Updated practical recommendation**: for large unsorted datasets, `minmax_lttb` is the best choice, with a 7.5x speedup on 1M shuffled rows and correct results. For sorted data, `lttb_sorted` remains slightly faster, 10 ms vs 11 ms.

### 7.6 Multi-Group GROUP BY, 100 Groups and 1M Total Rows

| Function | Time (ms) |
|---|---|
| `lttb` | 11.0 |
| `lttb_sorted` | 8.0 |
| `lttb_indices` | 11.0 |
| `minmax_lttb(r=4)` | 10.0 |
| `bucket_stats` | 11.0 |

### Analysis

- `lttb_sorted` is still fastest in multi-group workloads, 8 ms vs 11 ms, a 1.38x speedup.
- `minmax_lttb` is slightly faster than `lttb` in multi-group workloads, 10 ms vs 11 ms, because each group is smaller, about 10K rows, and preselection overhead is lower.
- All functions are faster in the multi-group case than a single 1M-row group, 11 ms vs 15 ms, because each group is smaller and the combine pointer-transfer optimization is effective.

### 7.7 Function Selection Guide

| Scenario | Recommended Function | Reason |
|---|---|---|
| Sorted input, one-shot downsampling | `lttb_sorted` | Removes sorting cost, 1.67x speedup |
| Unsorted input, one-shot downsampling | `minmax_lttb` | Bin-first strategy skips the full sort, 7.5x speedup on 1M shuffled rows |
| Unsorted input, exact result required | `lttb` | Sorts internally and returns exact LTTB output, but sorting cost is high |
| Sorted input, approximate result acceptable | `minmax_lttb_sorted` | Bin-first fast path, similar performance to `minmax_lttb` |
| Selected-point indices required | `lttb_indices` | Same performance as `lttb`, returns selected indices |
| Distribution analysis rather than curve shape | `bucket_stats` | Provides min/max/mean/std statistics, useful for agent-driven analysis |
