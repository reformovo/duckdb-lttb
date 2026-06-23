# duckdb-lttb 架构文档

## 1. 项目概述

`duckdb-lttb` 是一个 DuckDB 扩展，实现了 **LTTB（Largest-Triangle-Three-Buckets）** 时间序列降采样算法。LTTB 由 Sveinn Steinarsson 在 2013 年的硕士论文中提出，是一种专为可视化设计的降采样方法：在将大量数据点缩减到少量点的同时，尽可能保持原始曲线的视觉特征。

本扩展将 LTTB 实现为 DuckDB 的**聚合函数（Aggregate Function）**，使其可以直接在 SQL 查询中使用，无需将数据导出到 Python/R 等外部环境。行为参考了 ClickHouse 的 `largestTriangleThreeBuckets` / `lttb` 实现，并在类型保持、排序稳定性、边界处理等方面做了改进。

### 提供的函数

| 函数 | 说明 | 返回类型 |
|---|---|---|
| `lttb(x, y, n)` | 降采样到 `n` 个点，内部按 `x` 排序后采样 | `STRUCT(x typed, y typed)[]` |
| `largestTriangleThreeBuckets(x, y, n)` | `lttb` 的别名 | 同上 |
| `lttb_sorted(x, y, n)` | 与 `lttb` 相同，但跳过排序。调用方需保证输入已按 `x` 排序 | 同上 |
| `lttb_indices(x, y, n)` | 与 `lttb` 相同的选点逻辑，但返回选中点的排序后位置索引 | `BIGINT[]` |
| `minmax_lttb(x, y, n, minmax_ratio)` | 两阶段 MinMax 预选 + LTTB。`minmax_ratio` 默认 4。近似算法，非精确 LTTB | `STRUCT(x typed, y typed)[]` |
| `minmax_lttb_sorted(x, y, n, minmax_ratio)` | 与 `minmax_lttb` 相同，但跳过排序。调用方需保证输入已按 `x` 排序 | 同上 |
| `bucket_stats(x, y, num_buckets)` | 每桶统计降采样，返回 y 的分布统计（count/min/max/mean/std/first/last） | `STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]` |

### 支持的输入类型

`x` 和 `y` 均接受以下类型：

- **浮点**：`DOUBLE`、`FLOAT`
- **有符号整数**：`TINYINT`、`SMALLINT`、`INTEGER`、`BIGINT`、`HUGEINT`
- **无符号整数**：`UTINYINT`、`USMALLINT`、`UINTEGER`、`UBIGINT`、`UHUGEINT`
- **时间**：`DATE`、`TIMESTAMP`、`TIMESTAMP_TZ`、`TIMESTAMP_S`、`TIMESTAMP_MS`、`TIMESTAMP_NS`
- **十进制**：`DECIMAL`

输出保持 `x` 和 `y` 的原始类型，匹配 ClickHouse `Array(Tuple(typed_x, typed_y))` 的语义。

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                      SQL 查询层                              │
│  SELECT lttb(x, y, 1000) FROM metrics WHERE project='p1'   │
│  SELECT minmax_lttb(x, y, 1000, 4) FROM metrics             │
│  SELECT bucket_stats(x, y, 100) FROM metrics                 │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              DuckDB 函数绑定层 (Bind)                        │
│                                                             │
│  LTTBBindFunctionImpl / MinMaxLTTBBindFunction / ...         │
│    ├─ 校验 x/y 类型是否在支持集合内                           │
│    ├─ SQLNULL → DOUBLE 强制转换（向后兼容）                   │
│    ├─ 设置 function.arguments 为具体类型                     │
│    ├─ 设置 function.SetReturnType 为 LIST(STRUCT(...))      │
│    ├─ MakeReader/MakeWriter 解析类型为函数指针               │
│    └─ 返回 FunctionData (携带 sorted 标志 + 函数指针)        │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              聚合执行层 (Aggregate Lifecycle)                │
│                                                             │
│  Initialize → Update → [Combine] → Finalize → Destroy       │
│                                                             │
│  Update:   x_read/y_read 函数指针将类型化输入转为 double     │
│  Combine:  指针转移或 reserve+insert 合并部分状态             │
│  Finalize: Downsample 执行 LTTB 算法                         │
│            (或 MinMax 预选 + LTTB / 桶统计)                  │
│            x_write/y_write 函数指针将 double 转回类型化输出   │
│  Destroy:  释放堆分配的 vector                               │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              LTTB 算法核心 (Downsample)                      │
│                                                             │
│  stable_sort → 分桶 → 逐桶选最大三角形面积点 → 返回采样结果   │
│  可选: skip_sort (lttb_sorted) / out_indices (lttb_indices) │
│                                                             │
│  MinMax 预选 (bin-first): 等宽 x 区间分桶 → 每桶 argmin/argmax(y) │
│  BucketStatsFinalize: 等计数分桶 → 每桶统计聚合              │
└─────────────────────────────────────────────────────────────┘
```

核心设计原则：**算法在 `double` 上运行，类型转换只发生在 I/O 边界**。这使得 LTTB 算法本身与输入类型完全解耦，类型保持成为一个独立的 I/O 关注点。类型分发在 bind 时解析为函数指针（`MakeReader`/`MakeWriter`），Update/Finalize 每行直接调用，避免循环内的 switch 分发。

---

## 3. 代码结构

整个扩展的实现集中在单个文件 `src/lttb_extension.cpp`（约 1390 行）中，按功能分为以下几个部分：

### 3.1 数据结构定义

```cpp
struct LTTBPoint { double x, y; };           // 算法内部的数据点
struct LTTBState { ... };                     // 聚合状态
struct LTTBFunctionData : public FunctionData { ... };  // Bind 数据（lttb 系列）
struct MinMaxLTTBFunctionData : public FunctionData { ... };  // minmax_lttb Bind 数据
struct BucketStatsFunctionData : public FunctionData { ... };  // bucket_stats Bind 数据
```

### 3.2 类型转换：函数指针分发

```cpp
using ReadFunc = double (*)(const UnifiedVectorFormat &, idx_t, const LogicalType &);
using WriteFunc = void (*)(Vector &, idx_t, double, const LogicalType &);
static ReadFunc MakeReader(const LogicalType &type);   // bind 时解析类型 → 读函数指针
static WriteFunc MakeWriter(const LogicalType &type);  // bind 时解析类型 → 写函数指针
static bool IsLTTBSupportedType(...);                   // 类型校验
```

类型分发在 bind 时解析为裸函数指针（trivially copyable，无状态捕获），存储在 FunctionData 中。Update/Finalize 每行直接调用函数指针，避免循环内的 20-case switch 分发。

### 3.3 LTTB 算法核心

```cpp
static std::vector<LTTBPoint> Downsample(...);              // 降采样核心逻辑
// MinMax 预选逻辑内联在 MinMaxLTTBFinalize 中（bin-first，见 §4.7）
```

### 3.4 聚合生命周期函数

```cpp
static void LTTBInitialize(...);          // 状态初始化
static void LTTBUpdate(...);              // 逐行更新状态（lttb/lttb_sorted/lttb_indices）
static void MinMaxLTTBUpdate(...);        // 逐行更新状态（minmax_lttb，4 参数）
static void BucketStatsUpdate(...);       // 逐行更新状态（bucket_stats）
static void LTTBCombine(...);             // 合并部分状态（所有函数共享）
static void LTTBFinalize(...);            // 最终化：执行降采样并写入结果
static void LTTBIndicesFinalize(...);     // 最终化：写入索引结果
static void MinMaxLTTBFinalize(...);      // 最终化：MinMax 预选 + LTTB
static void BucketStatsFinalize(...);     // 最终化：桶统计聚合
static void LTTBDestroy(...);             // 释放状态内存（所有函数共享）
```

### 3.5 函数注册与绑定

```cpp
static LogicalType LTTBReturnType(...);                        // 构造返回类型
static LogicalType BucketStatsReturnType(...);                 // bucket_stats 返回类型
static AggregateFunction GetLTTBConcreteFunction(...);         // lttb 具体函数
static AggregateFunction GetLTTBIndicesConcreteFunction(...);  // lttb_indices 具体函数
static AggregateFunction GetMinMaxLTTBConcreteFunction(...);   // minmax_lttb 具体函数
static AggregateFunction GetBucketStatsConcreteFunction(...);  // bucket_stats 具体函数
static unique_ptr<FunctionData> LTTBBindFunctionImpl(...);     // lttb 统一绑定逻辑
static unique_ptr<FunctionData> MinMaxLTTBBindFunction(...);   // minmax_lttb 绑定
static unique_ptr<FunctionData> BucketStatsBindFunction(...);  // bucket_stats 绑定
static void LoadInternal(...);                                 // 注册所有函数
```

### 3.6 扩展入口

```cpp
void LttbExtension::Load(...) { LoadInternal(loader); }
DUCKDB_CPP_EXTENSION_ENTRY(lttb, loader) { ... }
```

### 为什么用单文件实现

LTTB 扩展的全部逻辑约 1390 行，拆分为多个文件会引入不必要的头文件依赖和编译复杂度。单文件结构使得：
- 阅读者可以在一个文件中理解完整的执行流程（从 SQL 调用到结果输出）。
- 所有内部函数都在匿名命名空间中，无符号导出，编译器可以更好地内联优化。
- 编译依赖最小化——只需包含 DuckDB 的类型头和聚合函数头。

---

## 4. 核心实现详解

### 4.1 类型保持 I/O：MakeReader / MakeWriter

这是本扩展最核心的设计决策。LTTB 算法本身只关心 `double` 值的排序和三角形面积计算，但用户需要传入 `DATE`、`TIMESTAMP`、`DECIMAL` 等类型，并期望输出保持这些类型。

**解决方案**：在 I/O 边界做类型转换，算法内部始终操作 `double`。

```
用户输入 (DATE/TIMESTAMP/DECIMAL/...)
        │
        ▼  x_read() / y_read()  ← bind 时由 MakeReader 解析的函数指针
  double x, y  ──────►  LTTB 算法 (排序 + 分桶 + 选点)
        │
        ▼  x_write() / y_write()  ← bind 时由 MakeWriter 解析的函数指针
用户输出 (DATE/TIMESTAMP/DECIMAL/...)
```

**MakeReader** / **MakeWriter** 在 bind 时通过 `switch (type.id())` 解析类型，返回类型特定的裸函数指针（lambda 转换而来，trivially copyable，无状态捕获）。这些指针存储在 FunctionData 中，Update/Finalize 每行直接调用，避免了循环内的 20-case switch 分发。

| 类型 | 物理存储 | 转换方式 |
|---|---|---|
| `DATE` | `date_t` (int32 天数) | `Date::EpochDays()` → 天数 double |
| `TIMESTAMP` / `TIMESTAMP_TZ` | `timestamp_t` (int64 微秒) | `Timestamp::GetEpochMicroSeconds()` → 微秒 double |
| `TIMESTAMP_S` | `timestamp_sec_t` (int64 秒) | 直接读 `.value` → 秒 double |
| `TIMESTAMP_MS` | `timestamp_ms_t` (int64 毫秒) | 直接读 `.value` → 毫秒 double |
| `TIMESTAMP_NS` | `timestamp_ns_t` (int64 纳秒) | 直接读 `.value` → 纳秒 double |
| `DECIMAL` | int16/int32/int64/hugeint_t | 读原始整数 ÷ POW10[scale] |
| 整数类型 | 对应宽度整数 | `static_cast<double>` |
| `HUGEINT` / `UHUGEINT` | 128 位整数 | `Hugeint::TryCast` / `Uhugeint::TryCast`（检查返回值） |

**WriteFromDouble** 的逆操作由 `MakeWriter` 返回的函数指针执行。DECIMAL 写回时使用 `std::llround(value * POW10[scale])` 做四舍五入，并通过 `MaxValue`/`MinValue` clamp 到物理整数范围，防止 ULP 抖动导致的越界窄化。

**为什么用 bind 时函数指针而不是模板**：DuckDB 的 `first`/`last` 等聚合函数使用模板为每种类型生成独立的函数实例，因为它们的状态需要存储类型化值。但 LTTB 的状态始终是 `double`，不需要为每种类型生成独立的 Update/Combine/Finalize 函数。bind 时解析函数指针避免了模板膨胀（20+ 类型 × 2 = 40+ 实例化），且类型分发只在 bind 时执行一次，循环内是单次间接调用（现代 CPU 的分支预测器能很好地处理）。

**单调性保证**：所有支持的类型到 `double` 的转换都是单调的——即 `a < b` 在原始类型中成立，当且仅当转换后的 `double` 值保持同样的序关系。这保证了在 `double` 上排序的结果与在原始类型上排序的结果一致。代码中在排序处添加了注释说明这一不变量。

### 4.2 LTTB 算法：Downsample

`Downsample` 实现了 LTTB 降采样的核心逻辑，与 ClickHouse PR #57003 修复后的行为一致：

**算法步骤**：

1. **排序**（可跳过）：`stable_sort` 按 `x` 升序排列。使用稳定排序保证相同 `x` 值的点保持插入顺序。
2. **边界情况**：
   - `buckets == 0` → 返回空
   - `n <= buckets` → 返回全部排序后的点
   - `buckets == 1` → 返回第一个点
   - `buckets == 2` → 返回第一个和最后一个点
3. **分桶采样**（`buckets >= 3`）：
   - 桶宽度 = `(n - 2) / (buckets - 2)`
   - 第一个和最后一个点始终保留
   - 中间 `buckets - 2` 个桶，每个桶选一个使三角形面积最大的点
   - 三角形由三个点构成：**前一个选中点**、**当前候选点**、**下一个桶的平均点**
   - 面积公式：`|（前.x - 均值.x) * (候选.y - 前.y) - (前.x - 候选.x) * (均值.y - 前.y)|`

**空桶保护**：当下一个桶为空时（`next_end <= next_start`），使用最后一个点作为平均值。这是 DuckDB 实现相对于 ClickHouse 的一个防御性改进——ClickHouse 源码中没有这个保护，理论上会除以零（PR #57003 的分桶策略在实践中避免了这种情况，但源码层面缺少显式保护）。

**可选参数**：
- `skip_sort`：`lttb_sorted` 传入 `true`，跳过排序步骤。
- `out_indices`：`lttb_indices` 传入非空指针，函数会填充选中点在排序后数组中的 0-based 位置索引。

**为什么 Downsample 会修改输入**：当 `n <= buckets` 时，函数通过 `std::move(points)` 直接转移向量所有权，避免拷贝。这是安全的，因为 `LTTBFinalize` 是状态向量的终端消费者——调用后状态即被销毁。代码注释明确说明了这一约定。

### 4.3 聚合状态管理

DuckDB 聚合函数遵循 **Initialize → Update → [Combine] → Finalize → Destroy** 的生命周期：

**LTTBState**：
```cpp
struct LTTBState {
    std::vector<LTTBPoint> *points = nullptr;  // 堆分配的点向量
    uint64_t buckets = 0;                       // 请求的桶数
    bool has_buckets = false;                   // 是否已设置桶数
};
```

**为什么用裸指针 `new`/`delete` 而不是 `unique_ptr`**：DuckDB 聚合状态通过 `memcpy` 复制（状态大小由 `LTTBStateSize` 返回），`unique_ptr` 不可平凡复制。DuckDB 自身的聚合函数（如 `first`/`last`）也使用 `new char[]` + Destroy 清理的模式。Arena 集成曾作为 TODO 项评估，但 `std::vector` 与 Arena 的集成非平凡，且当前模式在 Destroy 中正确释放，因此推迟。

**LTTBUpdate**：
- 通过 `UnifiedVectorFormat` 统一处理不同向量格式（FLAT、CONSTANT、DICTIONARY 等）。
- 逐行检查有效性（NULL 跳过）、桶数合法性（非负、组内恒定）。
- 调用 `ReadAsDouble` 将类型化输入转为 `double`。
- `IsValidPoint` 检查 NaN（跳过 NaN 行，与 ClickHouse 一致）。
- 首次创建向量时 `reserve(256)`，避免早期几何增长（0→1→2→4→...→256）的多次重分配。
- 硬性上限 `MAX_LTTB_POINTS = 1ULL << 30`（与 ClickHouse 的 `MAX_ARRAY_SIZE` 一致）。

**LTTBCombine**——并行执行时合并部分状态：
- **指针转移优化**：当目标状态没有点时，直接转移源向量的指针（O(1)），并将源指针置空防止 Destroy 双重释放。
- **reserve + insert**：当目标已有数据时，先 `reserve(target.size + source.size)` 再 `insert`，避免每次合并触发几何重分配。
- 合并策略是简单拼接——排序在 Finalize 时统一进行，不需要有序合并。

**LTTBFinalize**：
- 从 `bind_data` 读取 `sorted` 标志（`lttb_sorted` 传入 `true`）。
- 对每个状态调用 `Downsample`。
- 通过 `WriteFromDouble` 将 `double` 结果转回类型化输出。
- 使用 `ListVector::Reserve` 管理输出向量容量。

**LTTBIndicesFinalize**：
- 与 `LTTBFinalize` 类似，但调用 `Downsample` 时传入 `out_indices` 指针。
- 输出 `BIGINT[]` 而非 `STRUCT(x, y)[]`。

**LTTBDestroy**：`delete state.points`，释放堆内存。

### 4.4 Bind 函数与多态分发

DuckDB 的聚合函数绑定机制允许在查询编译时根据输入参数类型动态选择具体实现。

**两阶段注册**：

1. **占位函数**（`GetLTTBFunction` 等）：注册时使用 `LogicalType::ANY` 作为 x/y 参数类型和返回类型，绑定回调设为 `LTTBBindFunction` 等。DuckDB 在查询绑定时调用这个回调。

2. **具体函数**（`GetLTTBConcreteFunction` 等）：绑定回调内部根据实际输入类型构造具体函数，设置正确的参数类型和返回类型。

**LTTBBindFunctionImpl** 是统一的绑定逻辑：
- 校验所有参数类型已解析（`UNKNOWN` 抛 `ParameterNotResolvedException`）。
- 读取 x/y 的实际类型，SQLNULL 强制转为 DOUBLE（向后兼容无类型 NULL 字面量）。
- 校验类型在支持集合内，否则抛出清晰的错误信息。
- 根据 `sorted` 和 `indices` 标志选择对应的具体函数。
- 返回 `LTTBFunctionData(sorted)` 作为绑定数据，供 Finalize 读取。

**三个绑定变体**：
- `LTTBBindFunction`：`sorted=false, indices=false`（标准 `lttb`）
- `LTTBSortedBindFunction`：`sorted=true, indices=false`（`lttb_sorted`）
- `LTTBIndicesBindFunction`：`sorted=false, indices=true`（`lttb_indices`）

**为什么用单个绑定实现 + 标志位而不是三个独立实现**：三个变体共享 95% 的逻辑（类型校验、具体函数构造），只在 `sorted` 和 `indices` 两个布尔标志上有差异。提取为 `LTTBBindFunctionImpl` 避免了代码重复，且标志位的语义清晰。

### 4.5 FunctionData 与函数指针

```cpp
struct LTTBFunctionData : public FunctionData {
    bool sorted;
    ReadFunc x_read, y_read;    // bind 时解析的类型读函数指针
    WriteFunc x_write, y_write; // bind 时解析的类型写函数指针
    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other_p) const override;
};
```

`FunctionData` 是 DuckDB 的绑定数据基类，用于在绑定阶段携带信息到执行阶段。`Copy()` 和 `Equals()` 是 DuckDB 要求的虚函数，用于查询计划缓存和等价性检查。

**函数指针是裸指针**（trivially copyable，无状态捕获），所以 `Copy()` 的默认成员拷贝是正确的。代码注释明确标注：不要替换为 `std::function` 或有状态 lambda，否则会破坏 Copy 语义。

`sorted` 标志在 Finalize 中被读取，决定是否跳过排序。`x_read`/`y_read`/`x_write`/`y_write` 在 Update/Finalize 中被直接调用，避免循环内的类型 switch 分发。

**三个 FunctionData 变体**：
- `LTTBFunctionData`：`lttb`/`lttb_sorted`/`lttb_indices` 共用，携带 `sorted` 标志 + 4 个函数指针。
- `MinMaxLTTBFunctionData`：`minmax_lttb` 专用，额外携带 `minmax_ratio` 默认值。
- `BucketStatsFunctionData`：`bucket_stats` 专用，携带 4 个函数指针（无 `sorted` 标志，bucket_stats 总是排序）。

### 4.6 minmax_lttb / minmax_lttb_sorted

`minmax_lttb(x, y, n, minmax_ratio)` 是两阶段近似降采样：先用 MinMax 预选缩减候选集，再对候选集跑标准 LTTB。`minmax_ratio` 默认 4（匹配 plotly-resampler `MinMaxLTTB` 默认值），可通过第 4 参数覆盖（传 NULL 用默认值）。`minmax_lttb_sorted` 是其排序输入快速路径。

**状态编码**：`MinMaxLTTBUpdate` 需要在 `LTTBState`（仅有一个 `uint64_t buckets` 字段）中同时存储 `n_out` 和 `minmax_ratio`。采用位打包——低 32 位存 `n_out`，高 32 位存 `minmax_ratio`（ratio ≤ 2³¹）。Update 时校验组内 `n_out` 与 `minmax_ratio` 恒定，否则抛异常。

**退化分支**（在 `MinMaxLTTBFinalize` 中，bin-first 之前）：
- `n_out == 0` → 空输出
- `n <= n_out` → 全部点（经 `Downsample`，处理排序）
- `minmax_ratio <= 1` 或 `n / ratio <= n_out` → 预选无收益，直接标准 LTTB

### 4.7 Bin-first MinMax 预选优化

`MinMaxLTTBFinalize` 的核心路径采用 **bin-first** 预选，跳过对全部 n 个点的完整排序。

**旧方案**（已移除）：`stable_sort` 全部 n 个点（O(n log n)）→ 扫描排序后点集做每桶 argmin/argmax（O(n)）→ 对候选集跑 LTTB。完整排序主导了运行时（在打乱的 1M 输入上约占 88%：59ms 排序 vs 67ms 总计），而 O(n) 预选扫描只是用一个 O(n) pass 换另一个，相对普通 `lttb` 仅约 1.0x 加速。

**新方案**（bin-first）跳过完整排序：

1. **Step 1**（O(n)）：一次遍历找 `x_min`/`x_max` 及对应的首/末点索引 `first_idx`/`last_idx`。
2. **Step 2**（O(n)）：将所有内部点（排除首/末）按等宽 x 区间分入 `n_minmax_buckets = n_out * ratio / 2` 个桶，每桶保留 `argmin(y)` 与 `argmax(y)`。用预计算的 `inv_bin_width`（乘法代替除法）提升吞吐。
3. **Step 3**：候选集 = 首点 + 每桶 argmin/argmax + 末点。桶内 ≤2 点时全保留；否则按 x 序压入 min/max 以保证后续排序稳定。
4. **Step 4**（O(k log k)）：仅对候选集（k ≈ n_out·ratio，典型约 4000）排序，然后跑 LTTB（`skip_sort=true`）。

**复杂度**：O(n) + O(k log k)，其中 k = n_out·ratio/2，对比旧方案的 O(n log n) + O(n)。

**基准**（1M 点 / n=1000）：
- 打乱输入：75ms → 10ms（7.5x）
- 已排序输入：17ms → 11ms（1.5x，受限于未改变的 O(n) Update 累积）

**边界修复**：当所有点 x 相等时，`first_idx == last_idx`，直接压入末点会重复端点。代码以 `if (last_idx != first_idx)` 守卫，避免候选集中端点重复（否则 Downsample 会让首末同为同一点，丢失真实末点）。

**内存**：仍为 O(n)——所有点在 Update 中累积（DuckDB 聚合模型）。候选集是临时的，在本次 Finalize 调用内构建并丢弃。降至 O(n_out·ratio) 需要流式状态设计（见 `TODO.md` "True low-memory MinMax state"）。

### 4.8 bucket_stats

`bucket_stats(x, y, num_buckets)` 返回每桶的 y 分布统计：`STRUCT(bucket_start, bucket_end, count, min, max, mean, std, first, last)[]`。

- **分桶**：等计数按索引分桶（与 LTTB 同公式）。首末点为单例边界桶，内部按宽度 `(n-2)/(num_buckets-2)` 划分。
- **统计**：仅对 y 计算。`min`/`max`/`first`/`last` 保持 `y_type`；`mean`/`std` 为 DOUBLE。标准差用**总体标准差**（除以 N，非 N-1），用 `sum_sq/N - mean²` 计算并 `max(0, ...)` 防止浮点负方差。
- **退化**：`num_buckets == 0` → 空条目；`n <= num_buckets` → 每点一桶（count=1, std=0）；`num_buckets == 1` → 全部点单桶。
- **排序**：`bucket_stats` 无 sorted 变体，总是在 Finalize 中 `stable_sort`。

---

## 5. 实现中遇到的问题及解决方案

### 5.1 TIMESTAMP_SEC / TIMESTAMP_MS epoch 转换 Bug（严重）

**问题**：最初实现中，`TIMESTAMP_SEC` 和 `TIMESTAMP_MS` 使用了 `Timestamp::GetEpochSeconds()` 和 `Timestamp::GetEpochMs()` 来转换。但这两个函数假设输入是**微秒**单位的 `timestamp_t`，会分别除以 1,000,000 和 1,000。而 `timestamp_sec_t` 和 `timestamp_ms_t` 的 `.value` 字段**已经直接存储秒和毫秒**，不需要再除。

**后果**：输入 `TIMESTAMP_S '2023-01-01'`（epoch 秒 = 1,672,531,200）会被错误地除以 1,000,000 得到 1672，写回时再乘以 1,000,000 得到 1,672,000,000，被解释为约公元 52977 年。

**发现方式**：@oracle 在 P0 实现审查中发现。TIMESTAMP_NS 当时是正确的（使用了 `GetEpochNanoSeconds(timestamp_ns_t)` 重载，直接返回 `.value`），但 SEC 和 MS 没有对应的正确重载使用。

**解决方案**：直接读取物理类型的 `.value` 字段，绕过 epoch 转换辅助函数：

```cpp
case LogicalTypeId::TIMESTAMP_SEC:
    return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_sec_t>(data)[idx].value);
case LogicalTypeId::TIMESTAMP_MS:
    return static_cast<double>(UnifiedVectorFormat::GetData<timestamp_ms_t>(data)[idx].value);
```

写回时也直接构造对应的物理类型，不使用 `FromEpochSeconds` / `FromEpochMs`。

**教训**：DuckDB 的时间戳变体有不同的物理存储单位，不能假设所有变体都使用微秒。必须为每种变体使用正确的物理类型和转换方式。添加了 TIMESTAMP_SEC 和 TIMESTAMP_MS 的往返测试以防止回归。

### 5.2 SQLNULL 类型导致向后兼容破坏

**问题**：P0 实现将 bind 函数从"接受 ANY 并隐式转换为 DOUBLE"改为"显式校验类型集合"。但 `lttb(NULL, 1.0, 1)` 中的 `NULL` 字面量类型是 `SQLNULL`，不在支持集合内，导致抛出异常。原有测试中有一个 `lttb(NULL, 1.0, 1)` 的用例，之前能通过是因为 ANY→DOUBLE 的隐式转换。

**解决方案**：在 bind 函数中将 `SQLNULL` 强制转为 `DOUBLE`：

```cpp
const auto &x_resolved = x_type.id() == LogicalTypeId::SQLNULL ? LogicalType::DOUBLE : x_type;
```

这保持了无类型 NULL 字面量的向后兼容性，同时仍然拒绝 VARCHAR 等不支持的类型。

### 5.3 `%llu` 格式化的跨平台可移植性

**问题**：错误消息中使用 `%llu` 格式化 `idx_t`（即 `uint64_t`）。但在 LP64 Linux 上 `uint64_t` 是 `unsigned long`（应用 `%lu`），在 macOS 上是 `unsigned long long`（应用 `%llu`）。clang-tidy 的 `google-runtime-int` 检查也会对 `unsigned long long` 发出警告。

**解决方案**：改用 `std::to_string(MAX_LTTB_POINTS)` 字符串拼接，完全避免格式说明符与类型不匹配的问题：

```cpp
throw InvalidInputException("lttb aggregate state exceeded maximum point count of " +
                            std::to_string(MAX_LTTB_POINTS));
```

### 5.4 未使用的 `decimal.hpp` 头文件

**问题**：clang-tidy 报告 `decimal.hpp` 未被直接使用。`DecimalType::GetScale()` 和 `PhysicalType` 实际上定义在 `types.hpp` 中，已通过 `vector.hpp` 传递性包含。

**解决方案**：移除 `#include "duckdb/common/types/decimal.hpp"`。

### 5.5 Combine 性能：几何重分配

**问题**：原始 `LTTBCombine` 在 `insert` 前不 `reserve`，导致每次合并都可能触发向量的几何重分配。对于多分区并行执行的查询（许多部分状态需要合并），这会造成不必要的多次拷贝和重分配。

**解决方案**：两层优化：
1. **指针转移**：当目标状态为空时，直接转移源向量指针（O(1)），源指针置空防止双重释放。
2. **reserve + insert**：当目标已有数据时，先 `reserve(target.size + source.size)` 再 `insert`，确保单次分配。

### 5.6 测试文件语法错误

**问题**：SQLLogicTest 格式要求每个 `statement` / `query` 块之间有空行分隔。最初添加测试时，连续的 `statement ok` 块之间缺少空行，导致解析器将下一个 `statement` 关键字当作 SQL 的一部分，报 "syntax error at or near statement"。

**解决方案**：确保所有 `statement` / `query` 块之间都有空行分隔。

---

## 6. 设计决策与权衡

### 6.1 为什么算法在 double 上运行

LTTB 的核心是排序和三角形面积计算。在 `double` 上运行算法意味着：
- **算法实现只需一份**：不需要为每种输入类型模板化算法核心。
- **排序正确性有保证**：所有支持的类型到 `double` 的转换都是单调的，排序结果一致。
- **精度可接受**：
  - 微秒时间戳在 `double`（53 位尾数）中可无损表示直到约公元 2250 年。
  - DECIMAL 超过 15 位有效数字时会有精度损失，但 LTTB 是可视化降采样，1 ULP 的抖动对视觉效果无影响。

**代价**：类型转换在 I/O 边界有额外开销。但这个开销是 O(n) 的，而排序是 O(n log n)，转换不是瓶颈。

### 6.2 为什么用 stable_sort 而不是 sort

ClickHouse 使用不稳定的 `::sort`，相同 `x` 值的点的相对顺序未定义。DuckDB 使用 `std::stable_sort`，保证相同 `x` 值的点保持插入顺序。

**理由**：
- 确定性输出：相同查询多次执行结果一致，便于测试和调试。
- 用户直觉：用户通常期望相同时间戳的数据点保持数据库中的出现顺序。
- Python `lttb` 包完全拒绝重复 `x` 值，DuckDB 的稳定排序是更友好的中间方案。

**代价**：`stable_sort` 的时间复杂度为 O(n log²n)（最坏情况）vs `sort` 的 O(n log n)。对于 LTTB 的典型工作负载（万到百万级点），差异不显著。

### 6.3 为什么 combine 用指针转移而不是 move

`std::move` 一个 `std::vector` 会将源向量置空，但 DuckDB 的 `LTTBDestroy` 会在 combine 之后对源状态调用 `delete`。如果用 `std::move`，源指针指向的 vector 对象仍然存在（只是内容被移走），Destroy 中的 `delete` 仍然正确。

但直接指针转移更简洁：`target.points = source.points; source.points = nullptr;`。这完全避免了 vector 对象的移动构造/析构，且语义清晰——源放弃所有权，目标接管。Destroy 对 `nullptr` 的 `delete` 是安全的无操作。

### 6.4 为什么 arena 集成被推迟

ClickHouse 使用 Arena 分配器（`allocatesMemoryInArena() = true`）。评估后发现：
- DuckDB 的 `ArenaAllocator` 与 `std::vector` 的集成非平凡——vector 内部管理自己的内存，不能直接用 arena 分配。
- DuckDB 自身的聚合函数（如 `first`/`last`）也使用 `new`/`delete` 模式，不是所有聚合都用 arena。
- 当前模式在 Destroy 中正确释放，没有内存泄漏。
- combine 的指针转移优化已经改善了内存行为。

推迟到有明确需求（如生产环境报告内存压力）时再实现。

### 6.5 为什么 lttb_sorted 是独立函数而不是参数

考虑过两种方案：
1. **独立函数** `lttb_sorted(x, y, n)`：调用方明确表达"我保证输入已排序"。
2. **额外参数** `lttb(x, y, n, skip_sort := true)`：通过参数控制。

选择独立函数的理由：
- 语义清晰：函数名本身表达了前置条件。
- 不破坏现有 `lttb(x, y, n)` 的三参数签名。
- DuckDB 的函数注册机制天然支持多个函数名共享同一实现。
- 避免用户误传 `skip_sort=true` 但输入未排序导致的静默错误结果。

### 6.6 为什么 lttb_indices 返回排序后位置索引

`lttb_indices` 返回的是选中点在**排序后**数组中的 0-based 位置索引，不是原始插入顺序的行号。

**理由**：
- 排序后位置索引是算法内部的自然产物，不需要额外跟踪原始行号。
- 原始行号在并行 combine 场景下难以维护（不同分区的行号会冲突）。
- 排序后索引对于前端可视化足够有用——前端可以先将数据排序，然后用索引引用排序后的数据。

---

## 7. 与 ClickHouse / Python 的对比

| 维度 | DuckDB (本扩展) | ClickHouse | Python `lttb` |
|---|---|---|---|
| 内部排序 | 是，stable_sort | 是，不稳定 ::sort | 否，要求预排序 |
| 重复 x | 保留，稳定顺序 | 保留，顺序未定义 | 拒绝（校验器） |
| n=0/1/2 | 优雅处理 | 优雅处理 | ValueError |
| n > 输入 | 返回全部排序点 | 返回全部排序点 | ValueError |
| NaN | 跳过该行 | 跳过该行 | 拒绝（校验器） |
| Inf | 保留（与 CH/Python 一致） | 保留 | 不处理 |
| NULL | 跳过（validity mask） | 无显式处理 | N/A |
| 输入类型 | 数值 + 时间 + DECIMAL | 数值 + Date/DateTime | numpy 数组 |
| 输出类型 | 保持 x/y 类型 | 保持 x 类型 | 保持 dtype |
| 索引输出 | `lttb_indices` | 无 | 无 |
| 排序快速路径 | `lttb_sorted` | 无 | N/A（要求预排序） |
| MinMax 近似 | `minmax_lttb` | 无 | `MinMaxLTTB`（plotly-resampler） |
| 桶统计 | `bucket_stats` | 无 | 无 |
| 类型分发 | bind 时函数指针 | 模板 | N/A |
| 内存模型 | O(n)，new/delete | O(n)，Arena | O(n)，numpy |
| 最大点数 | 1 << 30 | 1 << 30 | N/A |
| Combine | 指针转移 + reserve | 拼接 + 排序 | N/A |
| 空桶保护 | 有（回退到最后一点） | 无（源码层面） | N/A |

**DuckDB 的改进**：稳定排序、空桶保护、NULL 处理、排序快速路径、索引输出、MinMax 近似路径、桶统计、bind 时函数指针分发。

**DuckDB 的待改进项**：Arena 集成、用户可配置内存上限、SIMD 三角形面积加速。

---

## 8. 测试策略

测试使用 DuckDB 的 SQLLogicTest 框架，集中在 `test/sql/lttb.test`（115 个断言）。

### 测试覆盖

| 类别 | 测试内容 |
|---|---|
| 基本功能 | ClickHouse 标准示例、n=0/1/2/20、n>输入、range 生成 |
| 类型保持 | 直接 DATE/TIMESTAMP/TIMESTAMPTZ/TIMESTAMP_S/MS/NS 输入、DECIMAL 往返、非对称 x/y 类型 |
| 边界情况 | NaN 跳过、NULL 跳过（DOUBLE 和类型化）、全无效输入、Inf 处理（+Inf/-Inf x） |
| 重复 x | 透传模式（n=输入数）、桶选择模式（n<输入数） |
| 错误处理 | 负 n、组内非恒定 n、VARCHAR 拒绝、HUGEINT 越界写回 |
| 并行/合并 | 3 组 GROUP BY 测试 combine 路径 |
| ClickHouse 回归 | PR #57003 桶边界回归测试 |
| lttb_sorted | 排序输入快速路径（DOUBLE 和 TIMESTAMP） |
| lttb_indices | 索引输出（n=4、n=0、n=1） |
| minmax_lttb | 小输入退化为标准 LTTB、NULL 默认 ratio、n=0、n>=输入、ratio<=1 错误、负 n 错误、ratio 恒定性错误、TIMESTAMP 类型保持 |
| bucket_stats | 3 桶 6 点统计、n=0、n=1、n>=输入、负 n 错误、TIMESTAMP 类型保持 |

### 测试方法

- **输出格式化**：使用 `list_transform` + `format` 将 STRUCT 列表转为字符串列表进行断言，避免浮点格式化差异。
- **时间格式化**：使用 `strftime` 将 TIMESTAMP 输出格式化为 `%Y-%m-%d` 字符串。
- **rowsort**：GROUP BY 测试使用 `rowsort` 排序模式，避免组间顺序不确定性。

---

## 9. 文件清单

```
duckdb-lttb/
├── src/
│   ├── include/
│   │   └── ltb_extension.hpp      # 扩展头文件（Extension 类声明）
│   └── lttb_extension.cpp         # 全部实现（约 1390 行）
├── test/
│   └── sql/
│       └── lttb.test              # SQLLogicTest 测试（115 断言）
├── docs/
│   ├── reference/
│   │   └── clickhouse-lttb.md     # ClickHouse 参考行为归档
│   ├── benchmark.md               # 性能基准测试结果
│   ├── UPDATING.md                # DuckDB 版本升级指南
│   └── architecture.md            # 本文档
├── scripts/
│   └── benchmark.sh               # 性能基准测试（三方对比 + 函数级对比）
├── CMakeLists.txt                 # 构建配置
├── extension_config.cmake         # 扩展配置
├── Makefile                       # 构建入口
├── README.md                      # 用户文档
└── TODO.md                        # 待办事项与优化路线图
```

---

## 10. 未来展望

根据 `TODO.md`，尚未实现或已推迟的功能：

1. **用户可配置内存上限**（推迟）：通过 PRAGMA 设置或 FunctionData 传递每组最大点数，用于生产环境的大查询保护。当前硬性上限 `1 << 30` 足够慷慨，推迟到生产用户报告需要更低上限时再实现。
2. **Arena 集成**（推迟）：将状态分配迁移到 DuckDB 的 ArenaAllocator，减少堆分配碎片。`std::vector` 与 Arena 的集成非平凡，且当前 `new`/`delete` 模式在 Destroy 中正确释放，推迟到有明确内存压力需求时再实现。
3. **SIMD 三角形面积加速**（推迟）：使用 AVX2（x86）/ NEON（ARM）同时处理 4 个候选点的三角形面积计算。推迟到 `lttb_sorted` 在 PulseOn 中落地且性能分析显示三角形面积循环仍占 >25% 总时间时再评估。
