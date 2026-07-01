# duckdb-lttb Architecture

## 1. Project Overview

`duckdb-lttb` is a DuckDB extension that implements the **LTTB (Largest-Triangle-Three-Buckets)** time-series downsampling algorithm. LTTB was introduced by Sveinn Steinarsson in his 2013 master's thesis. It is designed for visualization: it reduces a large number of data points to a smaller set while preserving the visual shape of the original curve as much as possible.

The extension exposes LTTB as DuckDB **aggregate functions**, so it can be used directly in SQL without exporting data to Python, R, or another external runtime. The behavior is based on ClickHouse's `largestTriangleThreeBuckets` / `lttb` aggregate, with additional work around type preservation, deterministic sorting, and edge-case handling.

### Provided Functions

| Function | Description | Return Type |
|---|---|---|
| `lttb(x, y, n)` | Downsample to `n` points after sorting by `x` internally. | `STRUCT(x typed, y typed)[]` |
| `largestTriangleThreeBuckets(x, y, n)` | Alias for `lttb`. | Same as above |
| `lttb_sorted(x, y, n)` | Same as `lttb`, but skips sorting. The caller must guarantee input is sorted by `x`. | Same as above |
| `lttb_indices(x, y, n)` | Uses the same point-selection logic as `lttb`, but returns selected 0-based positions in sorted order. | `BIGINT[]` |
| `minmax_lttb(x, y, n, minmax_ratio)` | Two-stage MinMax preselection plus LTTB. `minmax_ratio` defaults to 4. This is approximate, not exact LTTB. | `STRUCT(x typed, y typed)[]` |
| `minmax_lttb_sorted(x, y, n, minmax_ratio)` | Same as `minmax_lttb`, but assumes input is already sorted by `x`. | Same as above |
| `bucket_stats(x, y, num_buckets)` | Equal-count bucket statistics for y: count, min, max, mean, std, first, and last. | `STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]` |

### Supported Input Types

Both `x` and `y` accept:

- **Floating point**: `DOUBLE`, `FLOAT`
- **Signed integers**: `TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `HUGEINT`
- **Unsigned integers**: `UTINYINT`, `USMALLINT`, `UINTEGER`, `UBIGINT`, `UHUGEINT`
- **Temporal types**: `DATE`, `TIMESTAMP`, `TIMESTAMP_TZ`, `TIMESTAMP_S`, `TIMESTAMP_MS`, `TIMESTAMP_NS`
- **Decimal values**: `DECIMAL`

The output preserves the original `x` and `y` logical types, matching ClickHouse's `Array(Tuple(typed_x, typed_y))` style of behavior.

---

## 2. Architecture Overview

```text
+-------------------------------------------------------------+
|                         SQL layer                           |
|  SELECT lttb(x, y, 1000) FROM metrics WHERE project='p1'   |
|  SELECT minmax_lttb(x, y, 1000, 4) FROM metrics             |
|  SELECT bucket_stats(x, y, 100) FROM metrics                |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|                    DuckDB bind layer                        |
|                                                             |
|  LTTBBindFunctionImpl / MinMaxLTTBBindFunction / ...         |
|    - Validate that x/y types are supported                  |
|    - Coerce SQLNULL to DOUBLE for compatibility             |
|    - Set concrete function argument types                   |
|    - Set return type to LIST(STRUCT(...))                   |
|    - Resolve MakeReader/MakeWriter to function pointers     |
|    - Return FunctionData with flags and function pointers   |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|                 Aggregate lifecycle layer                   |
|                                                             |
|  Initialize -> Update -> [Combine] -> Finalize -> Destroy   |
|                                                             |
|  Update:   x_read/y_read convert typed input to double      |
|  Combine:  pointer transfer or reserve+insert               |
|  Finalize: Downsample runs LTTB                             |
|            or MinMax preselection + LTTB / bucket stats     |
|            x_write/y_write convert double back to typed out |
|  Destroy:  release heap-allocated vector                    |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|                    Algorithm layer                          |
|                                                             |
|  stable_sort -> bucketing -> max-triangle point selection   |
|  Optional: skip_sort for lttb_sorted, indices for lttb_indices |
|                                                             |
|  MinMax preselection: equal-width x bins -> argmin/argmax(y) |
|  Bucket stats: equal-count buckets -> statistical aggregate |
+-------------------------------------------------------------+
```

The central design principle is: **the algorithm runs on `double`, and type conversion happens only at the I/O boundary**. This decouples the LTTB algorithm from the input logical types and makes type preservation an independent input/output concern. Type dispatch is resolved at bind time into function pointers through `MakeReader` and `MakeWriter`; Update and Finalize call those pointers directly for each row, avoiding a type switch inside tight loops.

---

## 3. Code Structure

The implementation lives in a single file, `src/lttb_extension.cpp`, about 1480 lines, organized into the following sections.

### 3.1 Data Structures

```cpp
struct LTTBPoint { double x, y; };                    // Internal algorithm point
struct LTTBState { ... };                             // Aggregate state
struct LTTBFunctionData : public FunctionData { ... };       // lttb family bind data
struct MinMaxLTTBFunctionData : public FunctionData { ... };  // minmax_lttb bind data
struct BucketStatsFunctionData : public FunctionData { ... }; // bucket_stats bind data
```

### 3.2 Type Conversion Through Function Pointers

```cpp
using ReadFunc = double (*)(const UnifiedVectorFormat &, idx_t, const LogicalType &);
using WriteFunc = void (*)(Vector &, idx_t, double, const LogicalType &);
static ReadFunc MakeReader(const LogicalType &type);   // bind-time type -> reader
static WriteFunc MakeWriter(const LogicalType &type);  // bind-time type -> writer
static bool IsLTTBSupportedType(...);                  // type validation
```

Type dispatch is resolved at bind time into raw function pointers. These pointers are trivially copyable and capture no state, so they can be stored safely in `FunctionData`. Update and Finalize call the selected pointer directly for each row, avoiding repeated 20-case type switches inside execution loops.

### 3.3 LTTB Core

```cpp
static std::vector<LTTBPoint> Downsample(...);
static std::pair<double, double> NextBucketAverage(...);
static idx_t SelectMaxAreaPoint(...);
static LTTBBucketBounds ComputeLTTBBounds(...);
```

`Downsample` contains the main LTTB orchestration and delegates the per-bucket details to helpers so functions stay under the local 80-line guideline. MinMax preselection is orchestrated by `MinMaxLTTBDownsample`.

### 3.4 Aggregate Lifecycle Functions

```cpp
static void LTTBInitialize(...);
static void LTTBUpdate(...);
static void MinMaxLTTBUpdate(...);
static void BucketStatsUpdate(...);
static void LTTBCombine(...);
static void LTTBFinalize(...);
static void LTTBIndicesFinalize(...);
static void MinMaxLTTBFinalize(...);
static void BucketStatsFinalize(...);
static void LTTBDestroy(...);
```

All aggregate functions share `LTTBState`; only Update and Finalize vary by function family.

### 3.5 Binding and Registration

```cpp
static LogicalType LTTBReturnType(...);
static LogicalType BucketStatsReturnType(...);
static AggregateFunction GetLTTBConcreteFunction(...);
static AggregateFunction GetLTTBIndicesConcreteFunction(...);
static AggregateFunction GetMinMaxLTTBConcreteFunction(...);
static AggregateFunction GetBucketStatsConcreteFunction(...);
static unique_ptr<FunctionData> LTTBBindFunctionImpl(...);
static unique_ptr<FunctionData> MinMaxLTTBBindFunction(...);
static unique_ptr<FunctionData> BucketStatsBindFunction(...);
static void LoadInternal(...);
```

The public function names are registered with placeholder `ANY` argument types. Their bind callbacks validate actual types, build a concrete aggregate function with the resolved types, and return function-specific `FunctionData`.

### 3.6 Extension Entry Point

```cpp
void LttbExtension::Load(...) { LoadInternal(loader); }
DUCKDB_CPP_EXTENSION_ENTRY(lttb, loader) { ... }
```

### Why a Single File

The extension is small enough that splitting it into multiple translation units would add header dependencies and compile complexity without much benefit. The single-file structure has practical advantages:

- Readers can follow the full SQL-to-result path in one file.
- Internal helpers live in an anonymous namespace with no exported symbols.
- The compiler has good visibility for inlining.
- Build dependencies stay minimal.

---

## 4. Core Implementation Details

### 4.1 Type-Preserving I/O: MakeReader / MakeWriter

The LTTB algorithm only needs `double` values for sorting and triangle-area computation, but users can pass `DATE`, `TIMESTAMP`, `DECIMAL`, and integer types, and expect the output to preserve those types.

The solution is to convert at the boundary:

```text
User input (DATE/TIMESTAMP/DECIMAL/...)
        |
        v  x_read() / y_read(), resolved by MakeReader at bind time
  double x, y  --->  LTTB algorithm (sort + bucket + select)
        |
        v  x_write() / y_write(), resolved by MakeWriter at bind time
User output (DATE/TIMESTAMP/DECIMAL/...)
```

`MakeReader` and `MakeWriter` switch on `type.id()` during binding and return type-specific raw function pointers, implemented as stateless lambdas converted to function pointers. The pointers are stored in `FunctionData` and called directly during execution.

| Type | Physical Storage | Conversion |
|---|---|---|
| `DATE` | `date_t`, int32 days | `Date::EpochDays()` to double days |
| `TIMESTAMP` / `TIMESTAMP_TZ` | `timestamp_t`, int64 microseconds | `Timestamp::GetEpochMicroSeconds()` to double microseconds |
| `TIMESTAMP_S` | `timestamp_sec_t`, int64 seconds | Read `.value` directly |
| `TIMESTAMP_MS` | `timestamp_ms_t`, int64 milliseconds | Read `.value` directly |
| `TIMESTAMP_NS` | `timestamp_ns_t`, int64 nanoseconds | Read `.value` directly |
| `DECIMAL` | int16/int32/int64/hugeint_t | Raw integer divided by `POW10[scale]` |
| Integer types | Corresponding integer width | `static_cast<double>` |
| `HUGEINT` / `UHUGEINT` | 128-bit integer | `Hugeint::TryCast` / `Uhugeint::TryCast`, checked |

`WriteFromDouble` is handled by the function pointer returned from `MakeWriter`. DECIMAL output uses `std::llround(value * POW10[scale])` and clamps to the physical integer range with `MaxValue` / `MinValue`, preventing out-of-range narrowing caused by tiny ULP differences.

Bind-time function pointers are used instead of templates because the aggregate state always stores `double`, unlike DuckDB aggregates such as `first` or `last` whose state stores typed values. This avoids template expansion across 20+ logical types while keeping type dispatch out of the execution loop.

All supported conversions are monotonic: if `a < b` in the source type, then the converted double values keep the same order. This guarantees sorting on double is consistent with sorting on the original logical type.

### 4.2 LTTB Algorithm: Downsample

`Downsample` implements the LTTB core and follows the behavior of ClickHouse after PR #57003.

Algorithm steps:

1. **Sort**, unless skipped: `stable_sort` by `x` ascending using the shared `LessByX` comparator. Stable sorting keeps equal-`x` points in insertion order.
2. **Handle boundary cases**:
   - `buckets == 0`: return an empty result.
   - `n <= buckets`: return all sorted points.
   - `buckets == 1`: return the first point.
   - `buckets == 2`: return the first and last points.
3. **Sample buckets**, for `buckets >= 3`:
   - `ComputeLTTBBounds(bucket, n, width)` computes half-open current and next bucket index ranges with the equal-count formula `[floor(k * width) + 1, floor((k + 1) * width) + 1)`, clamped to valid ranges.
   - `NextBucketAverage(points, next_start, next_end)` computes the next bucket average `(x, y)` and falls back to the last point for an empty next bucket.
   - `SelectMaxAreaPoint(points, previous, start, end, avg_x, avg_y)` chooses the candidate with maximum triangle area. The triangle is formed from the previous selected point, the current candidate, and the next bucket average.

The triangle area expression is:

```text
abs((previous.x - avg_x) * (candidate.y - previous.y) -
    (previous.x - candidate.x) * (avg_y - previous.y))
```

The empty-bucket fallback is a defensive improvement over ClickHouse's source-level implementation, which lacks an explicit division-by-zero guard even though the current bucket strategy normally avoids it.

Optional behavior:

- `skip_sort`: `lttb_sorted` passes `true`.
- `out_indices`: `lttb_indices` passes a non-null pointer and receives selected 0-based positions in sorted order.

`Downsample` may move from the input vector when `n <= buckets`, because Finalize is the terminal consumer of the aggregate state's vector.

### 4.3 Aggregate State Management

DuckDB aggregate functions follow this lifecycle:

```text
Initialize -> Update -> [Combine] -> Finalize -> Destroy
```

`LTTBState` stores the data needed by every aggregate function:

```cpp
struct LTTBState {
    std::vector<LTTBPoint> *points = nullptr;
    uint64_t buckets = 0;
    bool has_buckets = false;
};
```

A raw pointer is used because DuckDB aggregate states are copied as plain memory according to `LTTBStateSize`; `unique_ptr` is not trivially copyable. DuckDB's own aggregate implementations use similar explicit allocation and Destroy cleanup patterns. Arena integration was evaluated and deferred because `std::vector` does not integrate trivially with DuckDB's `ArenaAllocator`, and the current Destroy path releases memory correctly.

`LTTBUpdate`:

- Uses `UnifiedVectorFormat` to handle FLAT, CONSTANT, DICTIONARY, and other vector formats uniformly.
- Skips NULL rows through validity masks.
- Validates bucket count: non-negative and constant within each group.
- Converts typed input to double through the bind-time reader function pointers.
- Skips NaN points, matching ClickHouse.
- Allocates the point vector lazily and reserves `kInitialPointReserve = 256` on first use.
- Enforces `MAX_LTTB_POINTS = 1ULL << 30`, matching ClickHouse's broad guard.

Shared helpers used by `LTTBUpdate`, `MinMaxLTTBUpdate`, and `BucketStatsUpdate`:

- `PushPoint(state, x, y, func_name)`: allocates the vector if needed, appends a valid point, and checks the maximum point count.
- `SetBucketCount(state, buckets, func_name)`: records and validates a constant per-group bucket count.
- `LessByX(lhs, rhs)`: shared comparator for `Downsample`, MinMax candidate sorting, and `bucket_stats`.

`LTTBCombine` merges partial aggregate states during parallel execution:

- If the target state has no points, it transfers the source vector pointer in O(1) and nulls the source pointer to avoid double free.
- If the target already has data, it reserves `target.size + source.size` before inserting, avoiding repeated geometric reallocations.
- States are concatenated; sorting happens once in Finalize.

`LTTBFinalize` reads the `sorted` flag from bind data, calls `Downsample`, converts the selected points back to typed output, and writes a DuckDB list result using `ListVector::Reserve`.

`LTTBIndicesFinalize` follows the same path but writes `BIGINT[]` indices instead of `STRUCT(x, y)[]`.

`LTTBDestroy` deletes the heap-allocated vector.

### 4.4 Bind Functions and Polymorphic Dispatch

DuckDB binding lets an aggregate choose a concrete implementation after the query's input types are known.

Registration uses two stages:

1. Placeholder functions use `LogicalType::ANY` for x/y argument types and attach a bind callback.
2. The bind callback validates actual types, builds the concrete aggregate function with the resolved argument and return types, and returns `FunctionData`.

`LTTBBindFunctionImpl` is the shared binder for the `lttb` family:

- Rejects unresolved parameter types with `ParameterNotResolvedException`.
- Reads actual `x` and `y` types and coerces untyped `SQLNULL` to `DOUBLE` for backward compatibility.
- Validates both types against the supported set.
- Chooses the concrete function based on `sorted` and `indices` flags.
- Returns `LTTBFunctionData` for Finalize.

The three variants are:

- `LTTBBindFunction`: `sorted=false`, `indices=false`.
- `LTTBSortedBindFunction`: `sorted=true`, `indices=false`.
- `LTTBIndicesBindFunction`: `sorted=false`, `indices=true`.

One shared implementation is used because the variants differ only in these flags; type validation and concrete-function construction are otherwise identical.

### 4.5 FunctionData and Function Pointers

```cpp
struct LTTBFunctionData : public FunctionData {
    bool sorted;
    ReadFunc x_read, y_read;
    WriteFunc x_write, y_write;
    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other_p) const override;
};
```

`FunctionData` carries bind-time information into execution. `Copy()` and `Equals()` are required by DuckDB for plan caching and equivalence checks.

The read/write function pointers are raw, trivially copyable pointers. A plain member copy is therefore correct. They should not be replaced with `std::function` or stateful lambdas without revisiting Copy semantics.

FunctionData variants:

- `LTTBFunctionData`: shared by `lttb`, `lttb_sorted`, and `lttb_indices`; carries the `sorted` flag and four function pointers.
- `MinMaxLTTBFunctionData`: used by `minmax_lttb`; also carries the default `minmax_ratio`.
- `BucketStatsFunctionData`: used by `bucket_stats`; carries four function pointers and no sorted flag because `bucket_stats` always sorts.

### 4.6 minmax_lttb / minmax_lttb_sorted

`minmax_lttb(x, y, n, minmax_ratio)` is a two-stage approximate downsampling function: it first preselects candidates through MinMax, then runs standard LTTB on the candidate set. `minmax_ratio` defaults to 4, matching plotly-resampler's `MinMaxLTTB` default, and can be overridden by the fourth argument. Passing NULL uses the default.

`minmax_lttb_sorted` is the sorted-input variant.

State encoding packs both `n_out` and `minmax_ratio` into `LTTBState::buckets`, which is a single `uint64_t`:

- Low 32 bits: `n_out`.
- High 32 bits: `minmax_ratio`, with ratio constrained to fit.

`MinMaxLTTBUpdate` validates that both values are constant within a group.

Degenerate paths in `MinMaxLTTBFinalize`:

- `n_out == 0`: empty output.
- `n <= n_out`: all points, through `Downsample` so sorting behavior is consistent.
- `minmax_ratio <= 1` or `n / ratio <= n_out`: no useful preselection, so use standard LTTB.

### 4.7 Bin-First MinMax Preselection

`MinMaxLTTBFinalize` is a small orchestrator: for each group it unpacks `n_out` and `minmax_ratio`, calls `MinMaxLTTBDownsample`, and writes typed results.

The core algorithm uses **bin-first** preselection and avoids sorting all `n` points.

The removed old approach was:

```text
stable_sort all n points -> scan sorted points for per-bucket argmin/argmax -> run LTTB on candidates
```

That full sort dominated runtime. On shuffled 1M input it was about 88% of total time, around 59 ms of 67 ms.

The current bin-first approach:

1. **Step 1, O(n)**: `FindXRangeEndpoints` scans once to find `x_min`, `x_max`, and the corresponding first and last endpoint indices.
2. **Step 2, O(n)**: `BuildMinMaxBins` assigns all interior points to equal-width x-bins, excluding the endpoints. Each `MinMaxBin` keeps `argmin(y)` and `argmax(y)`. A precomputed `inv_bin_width` replaces division with multiplication.
3. **Step 3**: `CollectMinMaxCandidates` builds the candidate set: first point, each bin's min/max points, then last point. Bins with two or fewer points keep all points; otherwise min/max are appended in x order for stable follow-up sorting.
4. **Step 4, O(k log k)**: only candidates are sorted with `LessByX`, then LTTB runs with `skip_sort=true`. Typically `k` is about `n_out * ratio`, for example around 4000.

Complexity is O(n) + O(k log k), where `k ~= n_out * ratio / 2`, instead of the old O(n log n) + O(n).

Benchmarks on 1M points / n=1000:

- Shuffled input: 75 ms -> 10 ms, 7.5x.
- Sorted input: 17 ms -> 11 ms, 1.5x, limited by the unchanged O(n) Update accumulation.

Edge-case fix: when all x values are equal, `first_idx == last_idx`. Pushing the last endpoint unconditionally would duplicate the endpoint and cause `Downsample` to treat the first and last point as the same point. The code guards with `if (last_idx != first_idx)`.

Memory remains O(n) because DuckDB's aggregate state accumulates all points during Update. The candidate set is temporary and is discarded after the current Finalize call. Reducing state memory to O(n_out * ratio) would require a streaming state design; see `TODO.md`, "True low-memory MinMax state".

### 4.8 bucket_stats

`bucket_stats(x, y, num_buckets)` returns per-bucket y distribution statistics:

```text
STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]
```

`BucketStatsFinalize` handles degenerate branches and delegates statistics and result writing to helpers:

- **Bucketing**: equal-count index buckets use the same formula as LTTB. `ComputeBucketBounds` computes half-open ranges. The first and last points are singleton boundary buckets; interior points are split with width `(n - 2) / (num_buckets - 2)`.
- **Statistics**: `ComputeBucketStats(points, start, end)` scans once to compute count/min/max/mean/std/first/last for y. `min`, `max`, `first`, and `last` preserve `y_type`; `mean` and `std` are DOUBLE. Standard deviation is population standard deviation, dividing by N rather than N-1, and uses `sum_sq / N - mean^2` with `max(0, ...)` to guard against tiny negative floating-point variance.
- **Writing**: `WriteBucketStatsRow(sink, offset, row)` writes a `BucketStatsRow` into the result child vectors. `BucketStatsSink` holds references to the nine child vectors plus typed write pointers.
- **Degenerate cases**: `num_buckets == 0` returns an empty list; `n <= num_buckets` returns one bucket per point with count=1 and std=0; `num_buckets == 1` returns one bucket for all points.
- **Sorting**: there is no sorted variant for `bucket_stats`; it always uses shared `LessByX` and `stable_sort` in Finalize.

---

## 5. Implementation Issues and Fixes

### 5.1 TIMESTAMP_SEC / TIMESTAMP_MS Epoch Conversion Bug

The initial implementation used `Timestamp::GetEpochSeconds()` and `Timestamp::GetEpochMs()` to convert `TIMESTAMP_SEC` and `TIMESTAMP_MS`. Those helpers assume a microsecond-based `timestamp_t` input and divide by 1,000,000 or 1,000. However, `timestamp_sec_t` and `timestamp_ms_t` already store seconds and milliseconds directly in their `.value` field.

Consequence: `TIMESTAMP_S '2023-01-01'`, epoch seconds 1,672,531,200, was divided by 1,000,000 to 1672. Writing it back by multiplying again produced 1,672,000,000 microseconds, interpreted as a date around year 52977.

The fix is to read the physical `.value` directly:

```cpp
case LogicalTypeId::TIMESTAMP_SEC:
    return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_sec_t>(data)[idx].value);
case LogicalTypeId::TIMESTAMP_MS:
    return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ms_t>(data)[idx].value);
```

The writer also constructs the corresponding physical timestamp type directly instead of using `FromEpochSeconds` or `FromEpochMs`.

Lesson: DuckDB timestamp variants use different physical storage units. Each variant needs a conversion path that matches its physical type. Round-trip tests for `TIMESTAMP_SEC` and `TIMESTAMP_MS` guard against regression.

### 5.2 SQLNULL Backward Compatibility

The P0 implementation changed binding from "accept ANY and implicitly cast to DOUBLE" to "explicitly validate supported types". The literal `NULL` in `lttb(NULL, 1.0, 1)` has type `SQLNULL`, which is not in the supported-type set, so an existing test started failing.

The fix coerces untyped `SQLNULL` to `DOUBLE` in the binder:

```cpp
const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
```

This preserves backward compatibility for untyped NULL literals while still rejecting unsupported types such as VARCHAR.

### 5.3 Portable Formatting for idx_t

Error messages originally formatted `idx_t` with `%llu`. On LP64 Linux, `uint64_t` is `unsigned long`, so `%lu` would be correct; on macOS, `uint64_t` is `unsigned long long`, so `%llu` is correct. clang-tidy's `google-runtime-int` check also warns on `unsigned long long`.

The fix uses `std::to_string(MAX_LTTB_POINTS)` and string concatenation:

```cpp
throw InvalidInputException("lttb aggregate state exceeded maximum point count of " +
                            std::to_string(MAX_LTTB_POINTS));
```

### 5.4 Unused decimal.hpp Include

clang-tidy reported that `decimal.hpp` was not directly used. `DecimalType::GetScale()` and `PhysicalType` are available through existing DuckDB headers, so the direct include was removed.

### 5.5 Combine Reallocation Cost

The original `LTTBCombine` inserted source points without reserving capacity first, which could trigger repeated vector reallocations when many partial states were merged.

The fix has two layers:

1. **Pointer transfer**: if the target state is empty, transfer the source vector pointer in O(1) and null the source pointer.
2. **reserve + insert**: if the target already has data, reserve `target.size + source.size` before inserting, ensuring one allocation.

### 5.6 SQLLogicTest Syntax

SQLLogicTest requires a blank line between `statement` and `query` blocks. Early added tests omitted blank lines between consecutive `statement ok` blocks, causing the parser to read the next `statement` keyword as SQL text.

The fix is to keep blank lines between all SQLLogicTest blocks.

---

## 6. Design Decisions and Tradeoffs

### 6.1 Why the Algorithm Runs on double

The LTTB core is sorting plus triangle-area computation. Running the core on `double` means:

- **One algorithm implementation**: no templated algorithm core per input type.
- **Correct ordering**: all supported conversions are monotonic, so sorting order is preserved.
- **Acceptable precision**:
  - Microsecond timestamps fit exactly in double's 53-bit mantissa until roughly year 2250.
  - DECIMAL values with more than about 15 significant digits can lose precision, but LTTB is visual downsampling and 1-ULP jitter does not affect the visual outcome.

The cost is type conversion at the I/O boundary. That cost is O(n), while sorting is O(n log n), so conversion is not the bottleneck.

### 6.2 Why stable_sort Instead of sort

ClickHouse uses unstable `::sort`, so points with equal `x` can have undefined relative order. DuckDB uses `std::stable_sort`, preserving insertion order for equal `x` values.

Reasons:

- Deterministic output across repeated queries.
- Better match for user expectations around duplicate timestamps.
- Python `lttb` rejects duplicate x values entirely, so stable sorting is a more useful middle ground.

The cost is worst-case O(n log^2 n) for `stable_sort` vs O(n log n) for `sort`. For typical LTTB workloads from tens of thousands to millions of points, the measured difference is acceptable.

### 6.3 Why Combine Transfers Pointers Instead of Moving Vectors

Moving a `std::vector` would leave the source vector object valid but empty, and DuckDB's Destroy path would still delete the source vector object correctly. Direct pointer transfer is simpler:

```cpp
target.points = source.points;
source.points = nullptr;
```

This avoids vector move construction/destruction and makes ownership explicit: the source gives up ownership and the target takes it. Deleting `nullptr` in Destroy is safe.

### 6.4 Why Arena Integration Is Deferred

ClickHouse uses an Arena allocator through `allocatesMemoryInArena() = true`. For DuckDB, integration is not straightforward:

- `std::vector` owns its internal memory and cannot directly use DuckDB's arena without allocator work.
- DuckDB's own aggregate functions, such as `first` and `last`, also use explicit heap allocation patterns.
- The current Destroy path releases memory correctly.
- Combine pointer transfer already improves memory behavior.

Arena integration is deferred until there is concrete production pressure that justifies the complexity.

### 6.5 Why lttb_sorted Is a Separate Function

Two designs were considered:

1. Separate function: `lttb_sorted(x, y, n)`, where the function name states the precondition.
2. Extra parameter: `lttb(x, y, n, skip_sort := true)`.

The separate function was chosen because it is explicit, preserves the existing three-argument `lttb` signature, fits DuckDB's registration model, and reduces the chance that users accidentally pass `skip_sort=true` on unsorted input and get silently wrong results.

### 6.6 Why lttb_indices Returns Sorted Positions

`lttb_indices` returns selected 0-based positions in the sorted point array, not original insertion-order row numbers.

Reasons:

- Sorted positions are the natural byproduct of the algorithm.
- Original row numbers are difficult to maintain correctly through parallel combine, where different partitions can have conflicting local row positions.
- Sorted indices are useful enough for frontends: the frontend can sort the input data and use the indices to reference the sorted array.

---

## 7. Comparison with ClickHouse and Python

| Dimension | DuckDB, this extension | ClickHouse | Python `lttb` |
|---|---|---|---|
| Internal sorting | Yes, `stable_sort` | Yes, unstable `::sort` | No, requires pre-sorted input |
| Duplicate x | Preserved, stable order | Preserved, undefined order | Rejected by validator |
| n=0/1/2 | Gracefully handled | Gracefully handled | `ValueError` |
| n > input | Returns all sorted points | Returns all sorted points | `ValueError` |
| NaN | Skips row | Skips row | Rejected by validator |
| Inf | Preserved, consistent with CH/Python | Preserved | Not specially handled |
| NULL | Skipped via validity mask | No explicit handling | N/A |
| Input types | Numeric + temporal + DECIMAL | Numeric + Date/DateTime | NumPy arrays |
| Output types | Preserves x/y types | Preserves x type | Preserves dtype |
| Index output | `lttb_indices` | None | None |
| Sorted fast path | `lttb_sorted` | None | N/A, requires pre-sorted input |
| MinMax approximation | `minmax_lttb` | None | `MinMaxLTTB`, plotly-resampler |
| Bucket statistics | `bucket_stats` | None | None |
| Type dispatch | Bind-time function pointers | Templates | N/A |
| Memory model | O(n), new/delete | O(n), Arena | O(n), NumPy |
| Maximum point count | `1 << 30` | `1 << 30` | N/A |
| Combine | Pointer transfer + reserve | Concatenate + sort | N/A |
| Empty-bucket guard | Yes, fallback to last point | No explicit source-level guard | N/A |

DuckDB improvements include stable sorting, empty-bucket protection, NULL handling, a sorted fast path, selected-index output, MinMax approximation, bucket statistics, and bind-time function-pointer dispatch.

Remaining improvement areas include Arena integration, a user-configurable memory limit, and SIMD acceleration for triangle-area computation.

---

## 8. Test Strategy

Tests use DuckDB's SQLLogicTest framework and live in `test/sql/lttb.test`, currently around 125 assertions.

### Coverage

| Category | Coverage |
|---|---|
| Basic behavior | ClickHouse canonical example, n=0/1/2/20, n>input, range-generated input |
| Type preservation | Direct DATE/TIMESTAMP/TIMESTAMPTZ/TIMESTAMP_S/MS/NS input, DECIMAL round trips, asymmetric x/y types |
| Edge cases | NaN skipped, NULL skipped for DOUBLE and typed input, all-invalid input, Inf handling for +Inf/-Inf x |
| Duplicate x | Pass-through mode when n equals input size, bucket-selection mode when n is smaller |
| Error handling | Negative n, non-constant n within group, VARCHAR rejection, HUGEINT writeback overflow |
| Parallel/combine | 3-group GROUP BY test for the combine path |
| ClickHouse regression | PR #57003 bucket-boundary regression test |
| lttb_sorted | Sorted-input fast path for DOUBLE and TIMESTAMP |
| lttb_indices | Index output for n=4, n=0, and n=1 |
| minmax_lttb | Small-input fallback to standard LTTB, NULL default ratio, n=0, n>=input, ratio<=1 error, negative n error, ratio constancy error, TIMESTAMP type preservation |
| bucket_stats | 3-bucket 6-point statistics, n=0, n=1, n>=input, negative n error, TIMESTAMP type preservation |

### Test Methods

- **Output formatting**: use `list_transform` and `format` to convert lists of STRUCT values to string lists, avoiding floating-point formatting differences.
- **Time formatting**: use `strftime` to format TIMESTAMP output as `%Y-%m-%d`.
- **rowsort**: GROUP BY tests use SQLLogicTest `rowsort` mode to avoid relying on group output order.

---

## 9. File Map

```text
duckdb-lttb/
+-- src/
|   +-- include/
|   |   +-- lttb_extension.hpp      # Extension class declaration
|   +-- lttb_extension.cpp          # Full implementation, about 1480 lines
+-- test/
|   +-- sql/
|       +-- lttb.test               # SQLLogicTest suite, about 125 assertions
+-- docs/
|   +-- reference/
|   |   +-- clickhouse-lttb.md      # Archived ClickHouse reference behavior
|   +-- benchmark.md                # Benchmark results
|   +-- UPDATING.md                 # DuckDB version update guide
|   +-- architecture.md             # This document
+-- scripts/
|   +-- benchmark.sh                # Benchmarks: three-way comparison + function-level comparison
+-- CMakeLists.txt                  # Build configuration
+-- extension_config.cmake          # Extension configuration
+-- Makefile                        # Build entry point
+-- README.md                       # User documentation
+-- TODO.md                         # Backlog and optimization roadmap
```

---

## 10. Future Work

According to `TODO.md`, the following work is not implemented or has been deferred:

1. **User-configurable memory limit**, deferred: expose a PRAGMA or FunctionData setting for maximum points per group, useful as a production query guard. The current hard limit of `1 << 30` is intentionally generous; a lower configurable limit can wait for real production need.
2. **Arena integration**, deferred: move state allocation to DuckDB's `ArenaAllocator` to reduce heap fragmentation. Integrating `std::vector` with Arena is non-trivial, and the current `new`/`delete` path releases memory correctly.
3. **SIMD triangle-area acceleration**, deferred: use AVX2 on x86 or NEON on ARM to evaluate four candidate triangle areas at once. Revisit after `lttb_sorted` is deployed in PulseOn and profiling shows the triangle-area loop is still more than 25% of total runtime.
