# LLVM Width Optimization

This repository contains an out-of-tree LLVM pass plugin that experiments with
reducing integer width-changing instructions in LLVM IR.

The long-term goal is to minimize the number of `zext`, `sext`, and `trunc`
operations in a function while preserving semantics. This is not purely a
narrowing problem: in some cases, widening part of the IR can reduce the total
number of conversions.

The detailed design is in
[width-minimization-design.md](./width-minimization-design.md). This README
summarizes the current implementation and how to work with it.

## Current Status

The repository currently contains:

- a loadable LLVM pass plugin, `WidthOpt`
- a component analysis that groups equal-width SSA regions
- a candidate-width analysis that infers alternative widths from ext/trunc use
  patterns
- a first width-plan analysis that chooses per-component widths with a simple
  graph-labeling heuristic
- planner-side compare affinities so `icmp` operands can influence width
  choice directly
- several conservative local rewrites that already reduce width changes on
  specific patterns
- conservative plan-driven widening rewrites for small width-polymorphic
  components
- a lit regression suite, a broader baseline corpus, and an Alive2 validation
  script

The current optimizer is still conservative, but it is no longer purely local.
The global plan now drives widening of small width-polymorphic components and
uses compare affinities to influence width choices.

## Implemented Local Rewrites

`width-opt` currently handles:

- compare shrinking for selected extension patterns
  - `zext/zext`
  - `sext/sext`
  - mixed `sext/zext` for signed predicates
- `icmp` + `select` to `llvm.smin/smax/umin/umax`
- `phi` shrinking for `zext` or `sext` inputs plus fitting constants
- `select` shrinking for `zext` or `sext` arms plus fitting constants
- `sext` to `zext nneg` when `LazyValueInfo` proves the operand non-negative
- widening `icmp eq/ne` over matching `trunc`s when known bits prove the
  truncated-away high bits are zero
- plan-driven widening of components built from `phi`, `select`, `freeze`,
  `and`, `or`, and `xor`
- per-edge boundary repair for widened components, including retargeting
  external `icmp` users when the widened representation is compatible

The current planner keeps `i1` components fixed. Boolean values are tracked
for analysis and printing, but they are outside the current global width
search space.

## Repository Layout

- `include/`, `lib/`: plugin source
- `test/`: lit regression tests for this plugin
- `tests/`: broader baseline corpus against stock LLVM
- `scripts/verify_with_alive2.py`: optimize each `.ll` test with `width-opt`
  and check correctness with Alive2
- `width-minimization-design.md`: design document

The two test directories have different roles:

- `test/` tracks behavior that this plugin already implements
- `tests/` tracks broader patterns and current LLVM behavior, including cases
  that this plugin does not yet handle

When something in `tests/` starts working in `width-opt`, it should be promoted
into `test/` immediately and turned into a checked plugin regression.

## Building

This project is modeled after `llvm-tutor` and expects an LLVM installation or
build tree that contains `LLVMConfig.cmake`.

Example:

```bash
cmake -S /Users/regehr/llvm-width-optimization \
  -B /Users/regehr/tmp/llvm-width-optimization-build \
  -DWO_LLVM_INSTALL_DIR=/Users/regehr/llvm-project/for-alive

cmake --build /Users/regehr/tmp/llvm-width-optimization-build -j4
```

## Running the Passes

Example invocation:

```bash
/Users/regehr/llvm-project/for-alive/bin/opt \
  -load-pass-plugin /Users/regehr/tmp/llvm-width-optimization-build/lib/libWidthOpt.dylib \
  -passes='width-opt' -S input.ll
```

Available debug/inspection passes:

- `print<width-components>`
- `print<width-candidates>`
- `print<width-plan>`

Example:

```bash
/Users/regehr/llvm-project/for-alive/bin/opt \
  -load-pass-plugin /Users/regehr/tmp/llvm-width-optimization-build/lib/libWidthOpt.dylib \
  -passes='print<width-plan>' -disable-output input.ll
```

## Testing

Run the plugin regression suite:

```bash
/Users/regehr/llvm-project/for-alive/bin/llvm-lit -sv \
  /Users/regehr/tmp/llvm-width-optimization-build/test
```

Run the stock-LLVM baseline corpus:

```bash
zsh /Users/regehr/llvm-width-optimization/tests/run-baseline.sh
```

Run Alive2 over both `test/` and `tests/` after optimizing each file with
`width-opt`:

```bash
python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py
```

By default, the script uses:

- `opt`: `/Users/regehr/llvm-project/for-alive/bin/opt`
- `alive-tv`: `/Users/regehr/alive2-regehr/build/alive-tv`
- plugin: `/Users/regehr/tmp/llvm-width-optimization-build/lib/libWidthOpt.dylib`

These can be overridden with `--opt-bin`, `--alive-tv`, and `--plugin`.

## Design Direction

The planned optimizer has two major phases:

1. Legality analysis
   Determine which widths are legal for each width component using structural
   rules plus analyses such as known bits, constant ranges, and demanded bits.

2. Global width selection
   Choose widths that minimize conversion boundaries across the function.

This is intentionally different from existing LLVM passes like InstCombine and
AggressiveInstCombine's `TruncInstCombine`: those are primarily local,
pattern-driven reducers, while this project is aimed at whole-function width
assignment.

## TODO

This is the authoritative implementation queue.

General policy:

- use the remaining corpus in `tests/` to choose what to implement next
- as soon as a `tests/` case is genuinely supported by `width-opt`, promote it
  into `test/` with explicit `FileCheck` coverage

Near-term items:

- add freeze-aware compare shrinking
  - target: `tests/yes-freeze-icmp-ult-zext-zext.ll`
- add `zext(trunc(x))` to mask formation
  - target: `tests/yes-zext-trunc-to-mask.ll`
- add mixed `sext`/`zext` equality compare narrowing
  - target: `tests/no-icmp-eq-sext-zext.ll`
- add range-aware compare retargeting under `assume`
  - target: `tests/yes-icmp-assume-trunc-zext.ll`
- extend demanded-bits/range-based `sext` to `zext` weakening
  - targets:
    - `tests/yes-sext-to-zext-demanded-bits.ll`
    - `tests/yes-sext-multiuse-to-zext.ll`

Later items:

- trunc-rooted narrowing through arithmetic and select
  - targets:
    - `tests/yes-trunc-add-zext-operand.ll`
    - `tests/yes-select-sext-trunc.ll`
    - `tests/yes-trunc-phi-loop.ll`
- range-driven op narrowing beyond compares
  - target: `tests/yes-udiv-range-narrowing.ll`
- richer global planning for multiple alternative wide widths
  - target: `tests/no-global-widening-two-wide-widths.ll`
