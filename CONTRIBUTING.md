# Contributing

## Correctness before optimization

Changes to matching, queueing, persistence, or shared-memory layout should state the invariant they preserve and add a test that would fail if the invariant were broken. A faster result is not acceptable when command order, fill quantity, audit identity, or replay behavior becomes ambiguous.

## Local validation

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Concurrency changes should also be tested in a separate TSan build. Shared-memory changes should run `test_ipc` in a normal build because fork/mmap behavior is distinct from in-process race detection.

## Code expectations

- C++20, no hidden global ownership, and no raw pointers in the shared-memory layout.
- Prices remain integer ticks.
- One writer per order book.
- Same-symbol command order must have a documented linearization point.
- Persistent formats are encoded explicitly; never write a compiler struct directly to disk.
- New protocol/file-format fields require a versioning and compatibility decision.
- Benchmark changes must validate correctness and disclose hardware/build conditions.
- Avoid new third-party dependencies unless they materially improve correctness or measurement.

## Pull requests

Include the design reason, changed invariants, test evidence, sanitizer evidence where relevant, and benchmark output only when the change is performance-related. Do not replace a measured unfavorable result with an expected or theoretical number.
