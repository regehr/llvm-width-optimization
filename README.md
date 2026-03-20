# LLVM Width Optimization

This repository contains an out-of-tree LLVM pass plugin that experiments with
reducing integer width-changing instructions in LLVM IR.

The long-term goal is to minimize the number of `zext`, `sext`, and `trunc`
operations in a function while preserving semantics. This is not purely a
narrowing problem: in some cases, widening part of the IR can reduce the total
number of conversions.

The detailed design is in
[width-minimization-design.md](./width-minimization-design.md). This README
is the source of truth for the current prototype: what is implemented, how it
is tested, and what the next engineering steps look like.

Mandatory scope rule: `WidthOpt` must remove `sext`, `zext`, and `trunc`
itself. Appending or relying on unrelated general cleanup passes such as
`InstSimplify`, `InstCombine`, or `AggressiveInstCombine` is not an acceptable
way to claim effectiveness. Those passes may be used only as debugging aids or
comparison baselines.

## Current Status

The repository currently contains:

- a loadable LLVM pass plugin, `WidthOpt`
- a component analysis that groups equal-width SSA regions
- a candidate-width analysis that infers alternative widths from ext/trunc use
  patterns
- a first width-plan analysis that chooses per-component widths with a simple
  graph-labeling heuristic and weighted boundary pressure
- planner-side compare affinities so `icmp` operands can influence width
  choice directly
- a growing set of conservative local rewrites for compare, trunc-rooted, and
  range-driven patterns
- conservative plan-driven widening rewrites for small width-polymorphic
  components
- a lit regression suite that now covers all former baseline corpus cases
- an Alive2 validation script, including a `--verbose` mode that prints source
  and optimized IR

The current optimizer is still conservative, but it is no longer purely local.
The global plan now drives widening of small width-polymorphic components and
uses compare affinities plus weighted repeated boundary pressure to influence
width choices. At the same time, a large fraction of the implemented behavior
currently comes from targeted local rewrites that complement the planner.

## Implemented Local Rewrites

`width-opt` currently handles:

- compare shrinking for selected extension patterns
  - `zext/zext`
  - `sext/sext`
  - mixed `sext/zext` for signed predicates
  - mixed `sext/zext` for `eq/ne`, with one extra distinguishing bit when
    needed
- `icmp` + `select` to `llvm.smin/smax/umin/umax`
- `phi` shrinking for `zext` or `sext` inputs plus fitting constants,
  including mixed narrow widths through a common intermediate width
- `select` shrinking for `zext` or `sext` arms plus fitting constants,
  including mixed narrow widths through a common intermediate width and direct
  repair of trunc/zero-compare users from the narrowed select
- freeze-aware compare shrinking via `freeze(cast x) -> cast(freeze x)`
- `sext` to `zext nneg` when `LazyValueInfo` proves the operand non-negative
- demanded-bits-style `sext` to `zext nneg`
  - single masked use
  - compatible multi-use shared `sext`
- widening `icmp eq/ne` over matching `trunc`s when known bits prove the
  truncated-away high bits are zero
- widening unsigned/equality compares from `trunc` operands when high bits are
  known zero
- `trunc(sext(x))` or `trunc(zext(x))` folding back to the source width or a
  narrower trunc of the original source
- `zext(trunc(x))` to low-bit masking
- trunc-rooted shrinking for:
  - low-bit-preserving `add/sub/mul/and/or/xor` expressions when removable
    boundary instructions pay for rebuilding the expression at the truncated
    width
  - `select`
  - simple loop-carried `shl` recurrences
- `udiv` retargeting when range facts and removable width changes make it
  profitable
- local widening of `zext -> add -> zext` chains to reuse an existing wider
  extension path
- plan-driven widening of components built from `phi`, `select`, `freeze`,
  `and`, `or`, and `xor`
- target-width-extension-aware sign selection for widened components so the
  internal representation matches the dominant removable boundary casts
- per-edge boundary repair for widened components, including retargeting
  external `icmp` users when the widened representation is compatible, plus
  select-arm splitting for unsigned compares that cannot use a sign-extended
  internal value directly

The current planner keeps `i1` components fixed. Boolean values are tracked
for analysis and printing, but they are outside the current global width
search space.

## Repository Layout

- `include/`, `lib/`: plugin source
- `test/`: lit regression tests for this plugin
- `scripts/verify_with_alive2.py`: optimize each `.ll` test with `width-opt`
  and check correctness with Alive2
- `width-minimization-design.md`: design document

All maintained regression cases now live under `test/`.

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

Run the plugin regression suite directly:

```bash
/Users/regehr/llvm-project/for-alive/bin/llvm-lit -sv \
  /Users/regehr/tmp/llvm-width-optimization-build/test
```

From a configured build tree, run the single CMake test target:

```bash
cmake --build /Users/regehr/tmp/llvm-width-optimization-build --target check
```

`check` runs both:

- the lit regression suite under `test/`
- the smoke sweep that runs `width-opt` over every `.ll` file under `test/`
  and reports crashes or hard failures

Run Alive2 over all `.ll` files under `test/` after optimizing them with
`width-opt`:

```bash
python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py
```

By default, the script uses:

- `opt`: `/Users/regehr/llvm-project/for-alive/bin/opt`
- `alive-tv`: `/Users/regehr/alive2-regehr/build/alive-tv`
- plugin: `/Users/regehr/tmp/llvm-width-optimization-build/lib/libWidthOpt.dylib`

These can be overridden with `--opt-bin`, `--alive-tv`, and `--plugin`.

Tests tagged with `no-alive` are skipped by this script. Use that tag only
for cases that Alive2 cannot currently model, such as unsupported EH forms.

Verbose mode prints the source and optimized IR text for each file:

```bash
python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py \
  --verbose
```

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

The long-term design calls for a small-label global optimization problem. The
current prototype does not solve that problem exactly yet: it uses a simpler
candidate graph plus iterative planning heuristic, then applies conservative
plan-driven rewrites where the implementation is ready.

## Future Work

The obvious next steps are no longer about filling out the original test
corpus. They are about broadening the optimizer while keeping the existing
proof and regression discipline.

- fix current implementation weaknesses that leave supported reductions on the
  table

### Concrete TODOs from a `lib/WidthOpt.cpp` review

- fix the planner cost model so it counts all boundary pressure instead of
  collapsing each component pair to a single unit-cost edge or compare
  affinity
- align planner-side movable components with what the executor can actually
  rebuild
  Today the analysis and planner can choose widths for components containing
  `icmp`, `zext`, `sext`, and `trunc`, but the plan consumer only realizes
  widenings for small width-polymorphic regions built from `phi`, `select`,
  `freeze`, `and`, `or`, and `xor`. That mismatch lets neighboring components
  optimize against width choices that never materialize, which directly weakens
  the final result.
- generalize the global planner beyond the current simple heuristic and binary
  candidate model
- broaden plan-driven rewrites to more instruction kinds than the current small
  width-polymorphic regions
- strengthen legality reasoning with more systematic known-bits, range, and
  demanded-bits integration

### Concrete TODOs from the LLVM precision oracle

- handle vector instructions
- broaden trunc-rooted shrinking beyond the current low-bit-preserving
  binops, `select`, and shift-recurrence cases
- add more direct profitability modeling so local widen/narrow decisions align
  better with the global objective
