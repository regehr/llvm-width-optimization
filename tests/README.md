# Width Optimization Tests

These tests are a design corpus for the proposed width-assignment pass.

Each `.ll` file starts with a comment describing whether current LLVM can
optimize the pattern when run as:

`/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S`

The `YES` cases document existing LLVM behavior that overlaps with the proposed
pass.

The `NO` cases document gaps that a future width-assignment pass may want to
cover, including cases that appear to need global reasoning or a willingness to
change widths in either direction.

`run-baseline.sh` is a lightweight harness for the current corpus. It:

- reads the top-of-file `YES` or `NO` expectation
- runs `opt -passes='default<O2>' -S`
- checks a small filename-based output pattern for each test

Invoke it as:

`zsh /Users/regehr/llvm-width-optimization/tests/run-baseline.sh`

This is intentionally lighter than a full `FileCheck` suite. Its purpose is to
keep the design corpus stable before the new pass exists.
