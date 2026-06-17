# ClickHouse LTTB Reference

This document archives the ClickHouse `largestTriangleThreeBuckets` behavior used as the reference for this DuckDB extension.

## Source

- Official implementation: `ClickHouse/ClickHouse/src/AggregateFunctions/AggregateFunctionLargestTriangleThreeBuckets.cpp`
- Official function name: `largestTriangleThreeBuckets`
- Alias: `lttb`
- Introduced by ClickHouse as an aggregate for Largest-Triangle-Three-Buckets time-series downsampling.

## ClickHouse SQL Shape

```sql
largestTriangleThreeBuckets(n)(x, y)
lttb(n)(x, y)
```

- `n` is the number of points in the output series.
- `x` is the x-coordinate, usually time or step.
- `y` is the y-coordinate, usually metric value.
- The return value is an array of `(x, y)` tuples.

## Semantics To Match

- Sort input points by `x` ascending before sampling.
- Skip a row when either `x` or `y` is `NaN`.
- If input size is less than or equal to `n`, return all valid points in sorted order.
- If `n = 0`, return an empty array.
- If `n = 1`, return the first sorted point.
- If `n = 2`, return the first and last sorted points.
- For `n >= 3`, keep the first and last points and choose one point from each middle bucket by maximum triangle area.

## Type Behavior

ClickHouse accepts `x` and `y` values that are numeric or date/time-like:

- Number
- Date
- Date32
- DateTime
- DateTime64

The current DuckDB extension implementation starts with numeric inputs and returns `STRUCT(x DOUBLE, y DOUBLE)[]`.

## Memory Model

ClickHouse stores the input sample in the aggregate state, then sorts and downsamples during finalization.

Important implications:

- The algorithm is not constant-memory.
- Memory grows with the number of valid input points per aggregate group.
- ClickHouse has a point-count guard around `1ULL << 30` stored points.
- This DuckDB extension follows the same broad model: collect points in aggregate state, sort, then apply LTTB.

## Canonical Example

Input:

```sql
CREATE TABLE largestTriangleThreeBuckets_test (x Float64, y Float64) ENGINE = Memory;
INSERT INTO largestTriangleThreeBuckets_test VALUES
    (1.0, 10.0), (2.0, 20.0), (3.0, 15.0), (8.0, 60.0), (9.0, 55.0),
    (10.0, 70.0), (4.0, 30.0), (5.0, 40.0), (6.0, 35.0), (7.0, 50.0);
```

ClickHouse query:

```sql
SELECT largestTriangleThreeBuckets(4)(x, y)
FROM largestTriangleThreeBuckets_test;
```

Expected output:

```text
[(1,10),(3,15),(9,55),(10,70)]
```

DuckDB extension equivalent:

```sql
SELECT lttb(x, y, 4)
FROM lttb_input;
```

The corresponding SQLLogicTest is archived in `test/sql/lttb.test`.

## ClickHouse Test Files

- `tests/queries/0_stateless/02842_largestTriangleThreeBuckets_aggregate_function.sql`
- `tests/queries/0_stateless/02842_largestTriangleThreeBuckets_aggregate_function.reference`
- `tests/queries/0_stateless/03096_largest_triangle_3b_crash.sql`
- `tests/queries/0_stateless/03096_largest_triangle_3b_crash.reference`

The bucket-strategy regression from PR `#57003` is covered inside the `02842_largestTriangleThreeBuckets_aggregate_function` test.
