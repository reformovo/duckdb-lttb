# AGENTS.md

Loaded every session. Supplement to mechanical constraints (lint/test/CI), not a replacement.

## Stack

C++17, DuckDB extension (v1.5.2), CMake + Ninja, SQLLogicTest.
Single-file implementation: `src/lttb_extension.cpp` (~1450 lines).
No external runtime/build dependencies.

## Architecture

```
src/
  include/lttb_extension.hpp   Extension class declaration
  lttb_extension.cpp           All implementation (LTTB, minmax_lttb, bucket_stats)
test/
  sql/lttb.test                SQLLogicTest (~115 assertions)
docs/                          Architecture, benchmark, reference docs
```

- Type dispatch via bind-time function pointers (not templates).
- All aggregate functions share `LTTBState`; only Update/Finalize differ.
- `MAX_LTTB_POINTS = 1ULL << 30` hard upper bound on input size.
- Registered SQL functions: `lttb`, `largestTriangleThreeBuckets`, `lttb_sorted`,
  `lttb_indices`, `minmax_lttb`, `minmax_lttb_sorted`, `bucket_stats`.

## Must always

- C++17 standard (`CMAKE_CXX_STANDARD 17`).
- Run `make format-check` before declaring a task done.
- Run `./build/release/test/unittest "test/sql/lttb.test"` after logic changes.
- New SQL functions or behavior changes need SQLLogicTest cases in `test/sql/lttb.test`.
- Functions < 80 lines; extract helpers if larger.

## Must never

- No `using namespace` in headers (enforced: `google-build-using-namespace`).
- No C-style casts (enforced: `cppcoreguidelines-pro-type-cstyle-cast`).
- No commented-out code.
- No raw `new`/`delete` — use DuckDB `unique_ptr`/`shared_ptr`.
- No inline `NOLINT` to suppress clang-tidy (WarningsAsErrors: '*').
- No modifying `duckdb/` or `extension-ci-tools/` submodules.

## Boundaries

- **Always**: run format-check + focused tests before declaring done.
- **Ask first**: changing DuckDB version (v1.5.2), modifying `CMakeLists.txt`,
  adding dependencies, changing `.clang-format`/`.clang-tidy`.
- **Never**: push to main, commit `build/`, disable lint rules inline, modify submodules.

## Commands

```sh
# Build (release, Ninja + ccache)
PATH="/opt/homebrew/opt/ccache/libexec:$PATH" GEN=ninja make

# Debug build
PATH="/opt/homebrew/opt/ccache/libexec:$PATH" GEN=ninja make debug

# Format (auto-fix)
make format

# Format check (CI-equivalent, no fix)
make format-check

# Static analysis (clang-tidy, all warnings are errors)
make tidy-check

# Full test (builds DuckDB + unittest first — slow)
make test

# Focused test (fast, after build)
./build/release/test/unittest "test/sql/lttb.test"
```

## Verify gate sequence

Run after every change, in order:

1. `make format-check` — formatting (fast, no build needed)
2. `make tidy-check` — static analysis (slow; at minimum before push)
3. `./build/release/test/unittest "test/sql/lttb.test"` — tests (after build)

Pre-commit hook runs format-check; CI re-runs format-check + tidy-check via
the `code-quality-check` job. See `CONTRIBUTING.md` for hook installation.

## Style rules (enforced by .clang-format / .clang-tidy)

- ColumnLimit: 120
- Tab indentation (width 4)
- PointerAlignment: Right (`int *foo`)
- SortIncludes: false
- Classes/Functions: CamelCase
- Variables/Members/Parameters/Namespaces: lower_case
- Macros/Static vars/Enum constants: UPPER_CASE
- All clang-tidy warnings are errors

## Definition of Done

- `make format-check` passes
- `make tidy-check` passes (zero warnings)
- `./build/release/test/unittest "test/sql/lttb.test"` passes
- New logic has SQLLogicTest cases
- Diff is explainable line-by-line

## Skills (loaded on-demand)

- Vibe coding workflow / AI coding constraints → follow `.agents/skills/reform/`
