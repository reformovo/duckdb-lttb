# LTTB 性能对比：DuckDB vs ClickHouse vs Python

## 测试环境

| 项目 | 版本/配置 |
|---|---|
| DuckDB | v1.5.3（本地编译，Release，ninja + ccache） |
| DuckDB lttb 扩展 | 本项目，C++ 原生聚合函数 |
| ClickHouse | v25.7.1.3997（`clickhouse-server:latest-alpine`） |
| Python lttb | 0.3.2（pip 安装，numpy 后端） |
| Python duckdb | 1.4.5（pip 安装） |
| NumPy | 1.26.4 |
| 运行环境 | macOS arm64（Apple Silicon） |
| ClickHouse 运行方式 | Apple Container（Linux/arm64 虚拟化框架） |
| DuckDB 运行方式 | 原生 macOS arm64 二进制 |
| 测试方法 | best of 5 runs |
| 数据集 | 合成正弦波 `sin(i/scale)`，DOUBLE 类型 |

### 计时方法说明

| 实现 | 计时方式 | 包含的开销 |
|---|---|---|
| DuckDB | CLI `.timer on` | 子进程启动 + SQL 解析 + 查询执行 |
| ClickHouse | `clickhouse-client --time` | 服务端查询执行（不含连接/传输） |
| Python | `time.perf_counter` | 数据获取(fetchall) + numpy 转换 + lttb.downsample |

> **注意**：DuckDB 的计时包含 CLI 子进程启动开销（约 1ms），ClickHouse 的计时是服务端纯查询时间。因此 DuckDB 的实际查询执行时间比表中数值更低。Python 的计时包含从 DuckDB 获取数据的 fetchall 开销，这是 Python UDF 方式的固有成本。

---

## 1. 已排序 DOUBLE 输入

三种实现对相同已排序正弦波数据降采样的耗时对比：

| 数据量 | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms) | DuckDB/CH |
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

### 分析

- **DuckDB 在所有规模下均最快**，ClickHouse 次之，Python 最慢。
- **10K-100K**：DuckDB 比 ClickHouse 快 1.5-2x，比 Python 快 3-48x。DuckDB 的 CLI 启动开销约 1ms，在此规模下占比较大但仍最快。
- **1M**：DuckDB 与 ClickHouse 接近持平（16ms vs 18ms，比值 0.88x），两者均为 C++ 原生实现，算法核心性能相当。Python 为 248ms，慢 15x。
- **Python 的 n_out 敏感性**：Python 在 n_out=10000 时显著变慢（100K 数据 96ms vs n_out=100 的 26ms），因为 `np.array_split` 和逐桶循环的开销随桶数增长。DuckDB 和 ClickHouse 对 n_out 不敏感。

---

## 2. 打乱输入（排序成本）

对随机打乱顺序的输入进行降采样，测试内部排序的成本：

| 数据量 | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms)* |
|---|---|---|---|---|
| 10K shuffled | 1000 | 1.0 | 2.0 | 9.9 |
| 100K shuffled | 1000 | 6.0 | 8.0 | 31.6 |
| 1M shuffled | 1000 | 74.0 | 85.0 | 244.5 |

> *Python `lttb` 包要求输入严格递增，无法处理打乱数据。此处 Python 使用已排序数据，仅作为参考基线。

### 分析

- **DuckDB 在打乱输入下仍快于 ClickHouse**：1M 打乱数据 74ms vs 85ms（0.87x）。
- **排序成本**（与已排序对比）：
  - DuckDB：1M 排序成本 = 74ms - 16ms = **58ms**（占总时间 78%）
  - ClickHouse：1M 排序成本 = 85ms - 18ms = **67ms**（占总时间 79%）
  - 两者排序成本接近，DuckDB 略优。
- **Python 无法处理打乱输入**——这是 DuckDB/ClickHouse 作为数据库聚合函数的核心优势：用户不需要预先排序数据。

---

## 3. TIMESTAMP / DateTime 输入

测试时间戳类型输入的类型转换开销：

| 数据量 | n_out | DuckDB (ms) | ClickHouse (ms) | Python (ms) |
|---|---|---|---|---|
| 100K TIMESTAMP | 1000 | 2.0 | 3.0 | 31.7 |
| 1M TIMESTAMP | 1000 | 16.0 | 17.0 | 254.7 |

### 分析

- **类型转换开销可忽略**：DuckDB 和 ClickHouse 的 TIMESTAMP/DateTime 输入与 DOUBLE 输入耗时几乎相同（2ms vs 2ms，16ms vs 16ms）。两者都在内部转换为 epoch double 进行计算。
- **Python** 的 TIMESTAMP 耗时与 DOUBLE 相近（254ms vs 248ms），因为 Python 版本将时间戳作为 double 获取，转换开销被 fetchall 主导。
- **DuckDB 保持输出类型**：`lttb(x, y, n)` 当 x 为 TIMESTAMP 时返回 `STRUCT(x TIMESTAMP, y DOUBLE)[]`，用户无需手动 `to_timestamp()` 重建。ClickHouse 同样保持类型。Python 保持 numpy dtype。

---

## 4. 多组 GROUP BY

测试 100 组并行降采样（每组 1K-10K 点）的 combine 路径性能：

| 数据量 | n_out/组 | DuckDB (ms) | ClickHouse (ms) |
|---|---|---|---|
| 100K (100组) | 100 | 2.0 | 4.0 |
| 1M (100组) | 100 | 12.0 | 21.0 |

> Python `lttb` 包是纯函数，不支持 SQL GROUP BY 聚合，无法参与此对比。

### 分析

- **DuckDB 在多组场景下优势显著**：1M 100 组时 12ms vs 21ms，快 **1.75x**。
- DuckDB 的 combine 指针转移优化（当目标组为空时 O(1) 转移源向量指针）在多组场景下有效减少了内存拷贝。
- 100K 100 组时每组仅 1000 点，DuckDB 2ms vs ClickHouse 4ms，快 2x。

---

## 5. 综合对比矩阵

| 维度 | DuckDB lttb | ClickHouse | Python lttb |
|---|---|---|---|
| **10K 已排序** | **1.0ms** | 2.0ms | 3.1ms |
| **100K 已排序** | **2.0ms** | 3.0ms | 25.6ms |
| **1M 已排序** | **16.0ms** | 18.0ms | 248ms |
| **1M 打乱** | **74.0ms** | 85.0ms | N/A (不支持) |
| **1M TIMESTAMP** | **16.0ms** | 17.0ms | 255ms |
| **1M 100组** | **12.0ms** | 21.0ms | N/A (不支持) |
| **内部排序** | 是 (stable_sort) | 是 (不稳定) | 否 (要求预排序) |
| **类型保持** | 是 | 是 | 是 (numpy dtype) |
| **GROUP BY** | 是 | 是 | 否 |
| **打乱输入** | 支持 | 支持 | 不支持 |
| **索引输出** | `lttb_indices` | 无 | 无 |
| **排序快速路径** | `lttb_sorted` | 无 | N/A |
| **实现语言** | C++ | C++ | Python (numpy) |

---

## 6. 结论

### 性能排名

1. **DuckDB lttb 扩展** — 在所有测试场景下最快
2. **ClickHouse largestTriangleThreeBuckets** — 紧随其后，1M 规模下接近持平
3. **Python lttb** — 显著慢于两者，且功能受限

### 关键发现

- **DuckDB vs ClickHouse**：两者均为 C++ 原生实现，1M 点时算法核心性能接近持平（16ms vs 18ms）。DuckDB 的优势主要来自：更低的进程启动开销（原生 macOS vs Linux 容器）、多组 combine 优化（指针转移，1.75x 优势）、以及更小的 n_out 敏感性。
- **C++ vs Python**：在 1M 点时，C++ 实现（DuckDB/ClickHouse）比 Python 快 **15x**。Python 的瓶颈在于 `fetchall` 数据序列化、numpy 数组转换、以及逐桶 Python 循环。
- **数据库聚合 vs 外部函数**：DuckDB 和 ClickHouse 作为数据库聚合函数，可以直接在 SQL 中使用 GROUP BY、WHERE 过滤、JOIN 等操作，无需将数据导出到外部环境。Python `lttb` 要求预排序且不支持 GROUP BY，在实际数据分析工作流中需要额外的数据搬运步骤。
- **排序成本**：对于打乱输入，排序占总时间的 ~78%（DuckDB 58ms/74ms，ClickHouse 67ms/85ms）。`lttb_sorted` 快速路径可完全消除此成本，适用于已知数据已排序的场景（如时间序列主键有序表）。

### 环境注意事项

- ClickHouse 运行在 Apple 虚拟化框架的 Linux 容器中，存在虚拟化开销。DuckDB 原生运行在 macOS 上。这一差异对 10K-100K 规模的计时有影响（进程启动/连接开销），但对 1M 规模的算法核心性能影响较小（两者接近持平）。
- DuckDB 计时包含 CLI 子进程启动（~1ms），ClickHouse 计时为服务端纯查询时间。因此 DuckDB 的实际查询执行时间比表中数值更低。
- Python 计时包含从 DuckDB 获取数据的 `fetchall` 开销，这是 Python UDF 方式的固有成本，在实际使用中无法避免。

---

## 7. 函数级性能对比（DuckDB 内部）

对 DuckDB lttb 扩展的所有注册函数在相同数据集上进行性能对比，测试环境同上。这部分由 `scripts/benchmark.sh` 的 Part B（Section 5-10）生成，只需 DuckDB + 扩展，不需要 ClickHouse 或 Python。

### 7.1 全函数对比（1M 已排序输入，n=1000）

| 函数 | 耗时 (ms) | 说明 |
|---|---|---|
| `lttb` | 15.0 | 排序 + LTTB 采样 |
| `lttb_sorted` | 9.0 | 跳过排序，直接 LTTB 采样 |
| `lttb_indices` | 15.0 | 排序 + LTTB，输出索引 |
| `minmax_lttb(r=4)` | 14.0 | MinMax 预选 + LTTB |
| `minmax_lttb(r=8)` | 15.0 | MinMax 预选（更大候选集）+ LTTB |
| `bucket_stats` | 14.0 | 等计数分桶 + 统计聚合 |

### 分析

- **`lttb_sorted` 是最快的降采样函数**：9ms vs 15ms（`lttb`），排序成本 6ms（占总时间 40%）。适用于已知数据已排序的场景（如时间序列主键有序表）。
- **`minmax_lttb` 在 1M 批处理场景下无计算优势**：15ms 与 `lttb` 相同。MinMax 预选阶段（O(n) 扫描 + 分桶）的开销与 LTTB 三角形循环的节省抵消。`minmax_ratio` 从 4 增到 8 无显著差异——预选的候选集更大但 LTTB 仍跑在候选集上。这与 oracle 评审的预测一致：minmax_lttb 的价值在于交互式可视化的多次重采样场景，而非数据库批处理的一次性降采样。
- **`bucket_stats` 性能与 `lttb` 相当**：15ms。虽然不需要三角形面积计算，但需要排序 + 逐桶统计聚合（min/max/mean/std），计算量与 LTTB 相近。
- **`lttb_indices` 与 `lttb` 性能相同**：索引输出的额外开销可忽略。

### 7.2 lttb vs lttb_sorted（排序成本）

| 数据量 | n_out | lttb (ms) | lttb_sorted (ms) | 排序成本 (ms) |
|---|---|---|---|---|
| 10K | 1000 | 0.0 | 1.0 | ~0 |
| 100K | 1000 | 2.0 | 1.0 | 1.0 |
| 1M | 1000 | 15.0 | 9.0 | 6.0 |

### 分析

- 排序成本随数据量线性增长：100K 时 1ms，1M 时 6ms。
- 对于已排序输入，`lttb_sorted` 提供 **1.67x 加速**（1M 场景）。
- 10K 规模下 CLI 启动开销（~1ms）主导计时，排序成本不可测量。

### 7.3 lttb vs minmax_lttb（预选计算优势）

| 数据量 | n_out | lttb (ms) | minmax_lttb (ms) | 加速比 |
|---|---|---|---|---|
| 100K | 100 | 2.0 | 2.0 | 1.00x |
| 100K | 1000 | 1.0 | 1.0 | 1.00x |
| 1M | 100 | 16.0 | 15.0 | 1.06x |
| 1M | 1000 | 15.0 | 15.0 | 1.00x |

### 分析

- **minmax_lttb 在典型批处理场景下无显著加速**。MinMax 预选的 O(n) 扫描 + 分桶开销与 LTTB 三角形循环的节省抵消。
- 1M/n=100 时有微弱优势（1.06x），因为 n_out 较小时 LTTB 三角形循环在候选集上的节省更明显。
- **结论**：minmax_lttb 的价值在于交互式可视化场景（同一数据集多次以不同 n_out 重采样），预选阶段只执行一次，后续重采样在候选集上快速运行。对于数据库的一次性批处理降采样，`lttb_sorted` 是更好的选择。

### 7.4 bucket_stats 性能

| 数据量 | 桶数 | 耗时 (ms) |
|---|---|---|
| 10K | 100 | 1.0 |
| 10K | 1000 | 1.0 |
| 100K | 100 | 2.0 |
| 100K | 1000 | 2.0 |
| 1M | 100 | 16.0 |
| 1M | 1000 | 15.0 |

### 分析

- `bucket_stats` 性能与 `lttb` 相当（1M 时 15ms vs 15ms）。虽然不需要三角形面积计算，但排序 + 逐桶统计聚合（min/max/mean/std/first/last）的计算量与 LTTB 相近。
- 桶数对性能无显著影响——统计聚合是 O(n) 线性扫描，与桶数无关。

### 7.5 打乱输入（排序成本验证）

| 数据量 | n_out | lttb (ms) | lttb_sorted (ms)* |
|---|---|---|---|
| 100K shuffled | 1000 | 6.0 | 1.0 |
| 1M shuffled | 1000 | 67.0 | 8.0 |

> *`lttb_sorted` 对打乱输入会产生错误结果，此处仅展示消除排序后的时间。

### 分析

- 打乱输入的排序成本显著：1M 时 67ms - 8ms = **59ms**（占总时间 88%）。
- 对比已排序输入的 6ms 排序成本，打乱输入的排序更贵（`stable_sort` 对无序数据的最坏情况 O(n log²n)）。
- **实践建议**：如果数据已按 x 排序（如时间序列主键有序表），使用 `lttb_sorted` 可从 67ms 降到 8ms（**8.4x 加速**）。

### 7.6 多组 GROUP BY（100 组，1M 总量）

| 函数 | 耗时 (ms) |
|---|---|
| `lttb` | 11.0 |
| `lttb_sorted` | 8.0 |
| `lttb_indices` | 11.0 |
| `minmax_lttb(r=4)` | 10.0 |
| `bucket_stats` | 11.0 |

### 分析

- 多组场景下 `lttb_sorted` 仍最快（8ms vs 11ms，1.38x 加速）。
- `minmax_lttb` 在多组场景下略快于 `lttb`（10ms vs 11ms）——每组数据量较小（10K），预选开销更低。
- 所有函数在多组场景下都比单组 1M 快（11ms vs 15ms），因为每组数据量小、combine 指针转移优化生效。

### 7.7 函数选择建议

| 场景 | 推荐函数 | 理由 |
|---|---|---|
| 已排序输入，一次性降采样 | `lttb_sorted` | 消除排序成本，1.67x 加速 |
| 未排序输入，一次性降采样 | `lttb` | 内部排序，结果正确 |
| 需要选中点索引 | `lttb_indices` | 与 `lttb` 性能相同，输出索引 |
| 交互式可视化，多次重采样 | `minmax_lttb` | 预选一次，后续重采样在候选集上快速运行 |
| 分布分析（非曲线形状） | `bucket_stats` | 提供 min/max/mean/std 统计，适合 AI agent 分析 |
| 大数据集 + 小 n_out | `minmax_lttb` | 1M/n=100 时有微弱优势（1.06x） |
