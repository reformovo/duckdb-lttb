# Testing this extension
This directory contains all tests for the `lttb` extension. The `sql` directory holds tests written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html).

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

For fast local iteration after building, run the focused LTTB test directly:

```bash
./build/release/test/unittest "test/sql/lttb.test"
```
