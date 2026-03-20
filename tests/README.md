# Width Optimization Tests

This directory now holds only the historical baseline harness for the project.

The original `.ll` design corpus has been fully promoted into
[`test/`](/Users/regehr/llvm-width-optimization/test) as `width-opt`
regressions.

`run-baseline.sh` remains here in case new external corpus files are added in
the future.

Invoke it as:

`zsh /Users/regehr/llvm-width-optimization/tests/run-baseline.sh`

At the moment this directory contains no `.ll` files, so the harness is
dormant.

That harness is intentionally lighter than a full `FileCheck` suite. Its
purpose is to keep an external baseline corpus stable before new patterns are
absorbed into the main regression suite.
