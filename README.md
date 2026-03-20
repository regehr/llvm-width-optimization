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
- a growing set of conservative local rewrites for compare, trunc-rooted, and
  range-driven patterns
- conservative plan-driven widening rewrites for small width-polymorphic
  components
- a lit regression suite that now covers all former baseline corpus cases
- an Alive2 validation script, including a `--verbose` mode that prints source
  and optimized IR
- a historical `tests/` harness kept around for future baseline cases

The current optimizer is still conservative, but it is no longer purely local.
The global plan now drives widening of small width-polymorphic components and
uses compare affinities to influence width choices. At the same time, a large
fraction of the implemented behavior currently comes from targeted local
rewrites that complement the planner.

## Implemented Local Rewrites

`width-opt` currently handles:

- compare shrinking for selected extension patterns
  - `zext/zext`
  - `sext/sext`
  - mixed `sext/zext` for signed predicates
  - mixed `sext/zext` for `eq/ne`
- `icmp` + `select` to `llvm.smin/smax/umin/umax`
- `phi` shrinking for `zext` or `sext` inputs plus fitting constants
- `select` shrinking for `zext` or `sext` arms plus fitting constants
- freeze-aware compare shrinking via `freeze(cast x) -> cast(freeze x)`
- `sext` to `zext nneg` when `LazyValueInfo` proves the operand non-negative
- demanded-bits-style `sext` to `zext nneg`
  - single masked use
  - compatible multi-use shared `sext`
- widening `icmp eq/ne` over matching `trunc`s when known bits prove the
  truncated-away high bits are zero
- widening unsigned/equality compares from `trunc` operands when high bits are
  known zero
- `zext(trunc(x))` to low-bit masking
- trunc-rooted shrinking for:
  - `add`
  - `select`
  - simple loop-carried `shl` recurrences
- `udiv` retargeting when range facts and removable width changes make it
  profitable
- local widening of `zext -> add -> zext` chains to reuse an existing wider
  extension path
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
- `tests/`: historical baseline harness and documentation for future corpus use
- `scripts/verify_with_alive2.py`: optimize each `.ll` test with `width-opt`
  and check correctness with Alive2
- `width-minimization-design.md`: design document

The two test directories have different roles:

- `test/` tracks behavior that this plugin implements
- `tests/` is reserved for future baseline/corpus cases; at the moment it does
  not contain any `.ll` files

The original external corpus has been fully promoted into `test/`.

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

Run the historical baseline harness after adding new external corpus files
under `tests/`:

```bash
zsh /Users/regehr/llvm-width-optimization/tests/run-baseline.sh
```

At the moment, `tests/` contains no `.ll` files, so this harness is dormant.

Run Alive2 over all `.ll` files under `test/` and any future `.ll` files under
`tests/` after optimizing them with `width-opt`:

```bash
python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py
```

By default, the script uses:

- `opt`: `/Users/regehr/llvm-project/for-alive/bin/opt`
- `alive-tv`: `/Users/regehr/alive2-regehr/build/alive-tv`
- plugin: `/Users/regehr/tmp/llvm-width-optimization-build/lib/libWidthOpt.dylib`

These can be overridden with `--opt-bin`, `--alive-tv`, and `--plugin`.

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
  The current planner deduplicates ordinary def-use edges and compare
  affinities down to one entry per component pair, then charges each mismatch
  as cost 1. If one component feeds several width-changing users in the same
  neighboring component, or several compares tie the same pair together, the
  plan still sees only one unit of pressure. This systematically understates
  the benefit of agreeing widths across heavily used boundaries.
- align planner-side movable components with what the executor can actually
  rebuild
  Today the analysis and planner can choose widths for components containing
  `icmp`, `zext`, `sext`, and `trunc`, but the plan consumer only realizes
  widenings for small width-polymorphic regions built from `phi`, `select`,
  `freeze`, `and`, `or`, and `xor`. That mismatch lets neighboring components
  optimize against width choices that never materialize, which directly weakens
  the final result.
- make the pass iterate to a fixed point instead of running each rewrite family
  once over a snapshot worklist
  Several currently supported transforms can expose other currently supported
  transforms later in the pass. In particular, compare shrinking runs before
  `phi` and `select` shrinking, so a newly narrowed `phi` or `select` can
  create an ext/ext compare shape that is never revisited. Re-running the
  local rewrites until they stabilize should recover these missed reductions.
- remove input-order sensitivity from `phi` shrinking
  The current `phi` shrinker gives up if it sees a constant incoming before it
  has seen the first extension incoming, even when the PHI is otherwise fully
  shrinkable. Since PHI incoming order is arbitrary, the same reducible PHI can
  optimize or fail purely based on edge ordering.
- relax `phi` and `select` shrinking so they can use a common intermediate
  width instead of requiring all extension arms to have the exact same narrow
  width
  The implementation already has a helper that can materialize an extension
  operand at any width between its narrow and wide forms, but the `phi` and
  `select` matchers still require all extension arms to agree on one exact
  narrow width. That leaves legal cases such as mixed `zext i8` and `zext i16`
  arms feeding one wider `phi` or `select` untouched even though shrinking to
  `i16` is already structurally supported.
- improve the widened-component internal sign policy so one target-width
  `zext` user does not block elimination of many target-width `sext` users, or
  vice versa
  The current widener chooses a sign-extended internal representation only when
  there are target-width `sext` consumers and zero target-width `zext`
  consumers; otherwise it defaults to zero-extension. Because existing target-
  width extensions are removed only when that exact internal choice matches
  them, this coarse policy can leave a large number of removable extensions in
  place.
- audit and either prove or remove the mixed `sext`/`zext` `icmp eq/ne`
  shrinking rule
  The current implementation and tests claim that equality across mixed sign
  and zero extensions can always be narrowed to the max of the source widths.
  That deserves a dedicated re-check because it appears suspicious when the two
  source widths are equal: for example, `sext i8 0x80` and `zext i8 0x80` are
  not equal at the wide width, but the narrowed `icmp eq i8` would report them
  equal. This is a correctness TODO rather than an effectiveness TODO, but it
  is important enough to keep visible in the near-term work list.

- generalize the global planner beyond the current simple heuristic and binary
  candidate model
- broaden plan-driven rewrites to more instruction kinds than the current small
  width-polymorphic regions
- strengthen legality reasoning with more systematic known-bits, range, and
  demanded-bits integration
- generalize trunc-rooted arithmetic shrinking beyond the current targeted
  helpers
- add more direct profitability modeling so local widen/narrow decisions align
  better with the global objective
- add new external baseline cases under `tests/` as fresh gaps are discovered,
  and keep promoting them into `test/` once supported
- make everything work for vector instructions
