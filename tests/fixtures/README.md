# Test fixtures

- `unparsable-history.snapshot.bin`: copied unchanged from loro-dev/loro
  (`crates/loro-wasm/tests/fixtures/`, tag `loro-v1.17.1`, MIT). A snapshot whose
  stored change block holding `1@1` is truncated, so reads that need that part of
  the history fail with a decode error. `test_c_only` uses it.
