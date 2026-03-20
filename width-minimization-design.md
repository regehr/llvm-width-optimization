# Design: Minimizing Width-Changing Instructions in LLVM IR

This document describes the architectural direction for the project and the
reasoning behind it. When it differs from the current implementation,
[README.md](./README.md) is the source of truth for prototype status,
supported transforms, and near-term engineering priorities.

## Goal

Design an LLVM pass that reduces the number of integer width-changing instructions in a function, such as `zext`, `sext`, and `trunc`, while preserving semantics.

This is not purely a narrowing problem. In some cases, the best way to reduce
the total number of width changes is to widen part of the IR so that fewer
conversions are needed overall.

The pass should be:

- conservative
- reasonably fast on large functions
- implementable as a simple out-of-tree LLVM pass

The key idea is to separate:

1. legality: which widths are legal for each retargetable part of the IR
2. optimization: which legal widths should be chosen globally to minimize conversion boundaries

This avoids rewrite search and should scale much better than trying many local transformations blindly.

## Prototype Status

The current out-of-tree prototype is intentionally narrower than the full
design, but it already exercises both the local and global parts of the plan.

Implemented today:

- local compare shrinking for selected ext/ext patterns
- local compare shrinking for mixed `sext`/`zext` `eq/ne`, including the extra
  distinguishing bit needed for equal-width mixed-sign cases
- local `icmp` + `select` to min/max canonicalization
- local `phi` and `select` shrinking for `zext`/`sext` inputs plus fitting
  constants, including mixed narrow widths through a common intermediate width
  plus direct repair of trunc and zero-compare users from narrowed selects
- `sext` to `zext nneg` when `LazyValueInfo` proves a non-negative operand
- demanded-bits-style `sext` to `zext nneg`, including a shared multi-use case
- widening of `icmp eq/ne` over matching `trunc`s when known bits justify it
- widening unsigned/equality compares from `trunc` operands when high bits are
  known zero
- local `zext(trunc(x))` to mask formation
- trunc-rooted shrinking for selected `add`, `select`, and shift-recurrence
  patterns
- range-driven `udiv` narrowing
- local widening of a `zext -> add -> zext` chain into a reused wider path
- plan-driven widening of small width-polymorphic components
  - currently `phi`, `select`, `freeze`, `and`, `or`, `xor`
- target-width-extension-aware sign selection inside widened components so the
  internal representation matches the dominant removable `zext`/`sext`
  boundary kind
- per-edge boundary repair for widened components
- planner-side compare affinities so `icmp` operands can pull component widths
  toward agreement
- weighted planner pressure for repeated def-use boundaries and repeated
  compare affinities between the same component pairs

Current policy boundaries:

- `i1` components are pinned and excluded from the global width search
- the generic widener currently supports only small width-polymorphic regions
- many arithmetic improvements still come from targeted local rewrites rather
  than the global planner
- implementation TODOs and current regression priorities live in `README.md`

## High-Level Strategy

The pass has two phases.

### 1. Legality analysis

Identify groups of values and instructions that want to share a width, then compute which widths are legal for each group.

This analysis should use two sources of information:

- semantic knowledge of width-polymorphic or conditionally shrinkable operations
- facts from existing analyses such as known bits, constant ranges, and demanded bits

### 2. Global width selection

Choose widths for those groups so that the number of edges crossing between different widths is minimized.

For a first implementation, each group should choose between:

- its original width
- one alternative legal width

That alternative width will often be narrower, but it may also be wider if
doing so reduces the total number of conversions to neighboring anchored or
shared users.

This turns the optimization problem into a binary graph labeling problem, which
can be solved exactly with a min-cut style formulation.

That exact binary formulation is still the long-term design target. The current
prototype uses a simpler candidate graph plus iterative local-improvement
heuristic, then applies plan-driven rewrites where the implementation supports
them.

## Anchors

Some widths cannot be changed and must be treated as fixed.

Examples:

- function arguments
- function return values
- loads and stores
- atomics
- calls to non-overloaded functions
- non-overloaded intrinsics
- instructions whose semantics are tied to a specific width
- operations involving pointer-width semantics such as `ptrtoint` and `inttoptr`

These anchors define hard boundaries for the optimization.

## Width Components

The pass should build width components: maximal SSA subgraphs whose integer values naturally want one shared width.

This is a graph compression step. It should be done before any expensive reasoning.

Examples of instructions that create equal-width constraints:

- `phi`
- `select` result and data operands
- `freeze`
- `and`, `or`, `xor`
- `icmp` operand pair

These can be collected efficiently with a union-find structure.

After this step, the function is viewed as:

- nodes: width components
- edges: uses between components that may require width changes
- labels: chosen widths for the components

## Legality Domain

For each width component `C`, maintain a profile:

```text
Profile(C) = {
  OrigW,              // original IR width
  Fixed,              // true if anchored at OrigW
  MinAny,             // smallest legal internal execution width
  MinZ,               // smallest width that can recreate OrigW by zext
  MinS,               // smallest width that can recreate OrigW by sext
  CandWidths          // small candidate set, usually {OrigW, AltW}
}
```

Interpretation:

- `MinAny`: the component can execute internally at this width
- `MinZ`: the original wide value is always representable as `zext` of a narrower value
- `MinS`: the original wide value is always representable as `sext` of a narrower value

The current profile is biased toward proving narrowing legality. Widening is
usually easier semantically, so the first implementation can treat widened
candidate widths as externally supplied choices, derived from neighboring
anchors and supported instruction widths, rather than trying to encode widening
in the same lattice.

This split matters because different boundaries require different extension kinds.

Examples:

- unsigned consumers naturally align with `MinZ`
- signed consumers naturally align with `MinS`
- internal width-polymorphic computation mostly cares about `MinAny`

The domain should be monotone. Conservative joins are simple:

- larger minimum widths are more constrained
- `INF` means impossible
- joins can be pointwise `max`

This makes the fixpoint iteration straightforward.

## Instruction Classes

The analysis should classify instructions into three broad categories.

### 1. Hard anchors

These stay at their original width.

Examples:

- arguments
- returns
- loads/stores
- calls with fixed signatures
- unsupported instructions
- pointer-sensitive instructions

Transfer rule:

- `Fixed = true`
- `MinAny = MinZ = MinS = OrigW`

### 2. Freely width-polymorphic instructions

These mostly just enforce equal-width relationships and do not inherently require a large width.

Examples:

- `phi`
- `select`
- `freeze`
- `and`, `or`, `xor`
- `icmp eq/ne` operands
- overloaded intrinsics that are genuinely width-parametric

Transfer rule:

- unify relevant operands and results into one width component

### 3. Conditionally shrinkable instructions

These can often be retargeted to different widths, but shrinking usually needs
semantic rules or analysis facts while widening is typically constrained by
instruction support and profitability.

Examples:

- `icmp ult/ule/ugt/uge`
- `icmp slt/sle/sgt/sge`
- `add`, `sub`, `mul`
- `shl`, `lshr`, `ashr`
- `udiv`, `urem`, `sdiv`, `srem`
- `zext`, `sext`, `trunc`

These need explicit legality rules.

## Structural Legality Rules

The legality analysis should not rely only on known-bits or range information. It should also know semantic facts about common extension and compare patterns.

This is important because some reductions are justified structurally.

### Example: compare narrowing without explicit known bits

The pattern:

```llvm
%x32 = sext i16 %x to i32
%y32 = zext i8 %y to i32
%c = icmp slt i32 %x32, %y32
```

can be narrowed to:

```llvm
%y16 = zext i8 %y to i16
%c = icmp slt i16 %x, %y16
```

This is a semantic fact about how signed and zero extension interact with signed comparison. It should be encoded directly as a structural rule.

### Useful initial rules

1. `icmp eq/ne (ext a), (ext b)`

If both operands are extensions of narrower values, the comparison can often be moved to the maximum of the source widths.

2. `icmp ult/ule/ugt/uge (zext a), (zext b)`

Unsigned compare can be narrowed to the maximum source width.

3. `icmp slt/sle/sgt/sge (sext a), (sext b)`

Signed compare can be narrowed to the maximum source width.

4. Mixed signed/unsigned extension patterns

Patterns like:

```llvm
icmp slt (sext x:iN -> W), (zext y:iM -> W)
```

can often be narrowed to width `max(N, M)`, interpreting `x` as signed and `y` as zero-extended to that width.

5. Min/max introduction as a separate rule

Patterns like:

```llvm
%c = icmp slt T %x, %y
%r = select i1 %c, T %y, T %x
```

should be recognized independently as:

```llvm
%r = call T @llvm.smax.T(%x, %y)
```

Introducing `smax` or `umax` should be independent from width-changing transforms. This keeps the optimization pipeline modular.

## Fact-Based Legality Rules

Structural rules should be combined with standard LLVM-style analyses.

### Known bits

If the high bits are provably zero above bit `k`, then:

- `MinZ <= k + 1`
- `MinAny <= k + 1`

If the high bits are provably sign-replicated above bit `k`, then:

- `MinS <= k + 1`
- `MinAny <= k + 1`

### Constant ranges

Unsigned ranges can lower `MinZ`.

Signed ranges can lower `MinS`.

Both can lower `MinAny`.

### Demanded bits

Demanded bits help prove that some arithmetic or logical operations do not need all of their original width.

This becomes especially important when adding arithmetic support.

## Conservative Scope for the First Version

The MVP scope should be frozen before implementation starts.

Version 1 should support only this restricted set of operations:

- `phi`
- `select`
- `freeze`
- `and`, `or`, `xor`
- `icmp eq/ne`
- `icmp ult/ule/ugt/uge`
- `icmp slt/sle/sgt/sge`
- `zext`, `sext`, `trunc`

Everything else should be treated as one of:

- an anchor
- a boundary the pass does not retarget
- future work

This is enough to:

- capture many common extension/compare patterns
- reproduce transformations like the motivating `slt` example
- keep the rewrite logic manageable

Version 1 should not try to retarget general arithmetic such as:

- `add`
- `sub`
- `mul`
- `shl`
- `lshr`
- `ashr`
- `udiv`
- `urem`
- `sdiv`
- `srem`

Those operations can still appear in the function, but unless they are covered
by a very specific structural rule, they should remain at their original width
in the first implementation.

## Why Widening Matters

If the true objective is to minimize the number of width-changing instructions,
then narrowing alone is not enough.

A component at `i16` that feeds several anchored `i32` users may require
multiple `zext`s if it stays narrow. Promoting that component to `i32` and
paying for one `trunc` on a rare narrow use can reduce the total number of
conversions.

So the pass should be understood as a width-assignment pass, not as a pure
type-shrinking pass.

This has two consequences:

- candidate widths must include some widening choices
- the optimizer should use a secondary tie-breaker that prefers narrower widths
  only when conversion counts are equal

## Global Optimization Problem

Once each component has a small legal set of candidate widths, choose widths globally to minimize conversion boundaries.

For the first implementation, each component should have at most two choices:

- `OrigW`
- `AltW`, where `AltW` may be smaller or larger than `OrigW`

If `AltW` is a narrowing choice, it can be computed conservatively as:

```text
FloorW = max(MinAny, local legality floor, representation floor)
```

where:

- `local legality floor` captures instruction-specific restrictions
- `representation floor` captures whether consumers require `MinZ` or `MinS`

If `AltW` is a widening choice, it should be chosen conservatively from widths
that already exist nearby, especially:

- anchored user widths
- anchored def widths
- widths required by supported non-polymorphic instructions

The first implementation should not invent arbitrary wider widths. It should
only reuse widths that are already present in the local neighborhood of the
component graph.

### Objective

Minimize the total cost of edges between components that need different widths.

Costs correspond to inserted conversions:

- `trunc`
- `zext`
- `sext`

For the MVP, all three conversion kinds should have equal unit cost:

```text
cost(trunc) = 1
cost(zext)  = 1
cost(sext)  = 1
```

The primary objective is:

1. minimize the total number of width-changing instructions inserted or left at component boundaries

The secondary objective is:

2. among equal-cost solutions, prefer smaller chosen widths

The tertiary objective is:

3. among equal-cost, equal-width solutions, prefer rewriting fewer components and instructions

If multiple assignments have the same conversion cost, prefer the one with
smaller widths. This preserves the useful bias toward narrow computation without
making narrowing the primary objective.

### Why binary first

If every component chooses between two labels, the problem becomes a binary
labeling problem and can be solved exactly by a min-cut style formulation.

The important point is that the second label need not always be a narrower
width. It may be a widening choice when that is what reduces the total number
of conversions.

This gives:

- good asymptotic behavior
- clean implementation
- exact optimization for the chosen model

The current prototype has not implemented this exact solver yet. It uses a
smaller planning heuristic over the same general component/candidate structure.

Later, a multi-width version could use:

- alpha-expansion
- greedy local improvement
- another small-label graph optimization technique

## Pass Pipeline

The pass should proceed roughly as follows.

### 1. Collect integer subgraph

Ignore unsupported instructions and non-integer values except where they anchor widths.

### 2. Build width components

Use union-find to merge values linked by equal-width constraints.

### 3. Initialize profiles

For each component:

- set `OrigW`
- set `Fixed`
- initialize `MinAny = MinZ = MinS = OrigW`

### 4. Run legality fixpoint

Iteratively refine profiles using:

- structural rules
- known bits
- constant ranges
- demanded bits

This should converge quickly because the narrowing facts only move downward and joins are monotone. Widening candidates can be added separately from local anchor context without complicating the main fixpoint.

### 5. Compute candidate widths

For each component:

- if `Fixed`, only `OrigW` is allowed
- otherwise compute one alternative width `AltW`
- `AltW` may be a narrowing width derived from legality facts
- `AltW` may be a widening width induced by nearby anchors or heavy-use boundaries
- candidate set is usually `{OrigW, AltW}`

### 6. Build optimization graph

Nodes are components.

Edges are value uses across components.

Edge costs model the number and kind of conversions needed under each label combination.

### 7. Solve

Long-term target:

- run a binary cut solver or equivalent exact binary labeling algorithm

Current prototype:

- run a simpler iterative local-improvement heuristic over the candidate graph

### 8. Rewrite IR

Retarget chosen components to their selected widths and insert boundary conversions where needed.

### 9. Cleanup

Run standard simplification passes such as:

- InstCombine
- DCE

## Pass Placement Assumptions

The first prototype should assume a simple placement in a short experimental
pipeline rather than trying to thread itself into the full LLVM pipeline in a
subtle way.

A reasonable starting point is:

```text
instcombine
correlated-propagation
width-opt
instcombine
dce
```

The rationale is:

- pre-cleanup exposes local canonical forms and range-driven facts
- the width pass then reasons over simplified IR
- post-cleanup removes temporary casts and re-canonicalizes the result

Version 1 should therefore be implemented as a standalone function pass that is
useful in a short fixed pipeline.

That remains a good experimental placement for the current out-of-tree plugin.
The prototype does not yet attempt to integrate itself into LLVM's default
mid-end pipeline.

## Pseudocode

```text
runOnFunction(F):
  KB = KnownBitsAnalysis(F)
  CR = ConstantRangeAnalysis(F)
  DB = DemandedBitsAnalysis(F)

  G = collectIntegerSubgraph(F)

  UF = UnionFind()
  CondNodes = []

  for each integer instruction I in F:
    Class = classifyInstruction(I)

    if Class == FreelyWidthPolymorphic:
      addEqualWidthConstraints(UF, I)
    else if Class == HardAnchor:
      recordAnchor(I)
    else if Class == ConditionallyShrinkable:
      CondNodes.push(I)

  Comps = materializeComponents(UF, F)

  for each component C in Comps:
    Profile[C].OrigW = originalWidth(C)
    Profile[C].Fixed = touchesAnchorOrUnsupportedUse(C)
    Profile[C].MinAny = Profile[C].OrigW
    Profile[C].MinZ = Profile[C].OrigW
    Profile[C].MinS = Profile[C].OrigW

  changed = true
  while changed:
    changed = false

    for each component C in Comps:
      changed |= refineFromConstants(C, Profile)
      changed |= refineFromKnownBits(C, KB, Profile)
      changed |= refineFromConstantRanges(C, CR, Profile)
      changed |= refineFromDemandedBits(C, DB, Profile)

    for each instruction I in CondNodes:
      changed |= applyStructuralRule(I, Profile)
      changed |= applyFactBasedRule(I, KB, CR, DB, Profile)

  for each component C in Comps:
    if Profile[C].Fixed:
      CandWidths[C] = { Profile[C].OrigW }
      continue

    NarrowW = computeFloorWidth(C, Profile[C])
    WideW = computeCeilWidthFromNeighbors(C, Profile)
    AltW = chooseBestAlternative(C, NarrowW, WideW, Profile)

    if AltW != Profile[C].OrigW:
      CandWidths[C] = { Profile[C].OrigW, AltW }
    else:
      CandWidths[C] = { Profile[C].OrigW }

  CutGraph = buildBinaryLabelGraph(F, Comps, CandWidths, Profile)
  Labels = solveBinaryCut(CutGraph)

  rewriteFunction(F, Comps, Labels, Profile)
  runCleanup(F)
```

## Example Structural Rule

One concrete rule for the motivating example:

```text
applyStructuralRule(I = icmp slt (sext x:iN -> W), (zext y:iM -> W)):
  T = max(N, M)
  Profile[component(x, y)].MinAny = min(Profile[component(x, y)].MinAny, T)
```

In practice, the rewrite also needs to remember how the narrowed operands are formed. The legal target width is not enough by itself. The pass should record a reconstruction plan for each conditional rule that succeeds.

For example:

- use `%x` directly as signed `iN`
- zero-extend `%y` to `iT`
- emit `icmp slt iT`

This suggests that the implementation should track not just a legal width, but also a legal reconstruction recipe for selected conditional nodes.

## Representation at Boundaries

When a chosen narrow component feeds a wider consumer, the pass must decide whether to use:

- `zext`
- `sext`
- `trunc`

This is where `MinZ` and `MinS` matter.

Examples:

- if a wide use only needs a zero-extended interpretation, prefer `zext`
- if a wide use only needs a sign-extended interpretation, prefer `sext`
- if neither is provably valid, the narrowing is illegal for that boundary

This is another reason to keep the first implementation conservative.

Symmetrically, when a chosen wide component feeds a narrower consumer, the pass
must decide whether a `trunc` is legal at that boundary and whether it preserves
the required interpretation for all downstream uses.

## Expected Complexity

The intended asymptotic shape is close to linear in practice, apart from the global cut solve.

Main costs:

- building components with union-find: near-linear
- legality fixpoint: roughly linear per iteration
- binary optimization: one min-cut solve on the component graph

This should be practical for large LLVM functions as long as:

- the candidate width set stays small
- instruction support is initially conservative
- expensive semantic checks are encoded as local transfer rules

## Main Risks

### 1. Weak legality analysis

If the legality analysis is too weak, the pass will only rediscover trivial extension motion and miss the interesting transforms.

### 2. Missing profitable widening cases

If the candidate generation logic only proposes narrower widths, the pass can
miss obvious wins where one widening avoids many repeated extensions.

### 3. Overly ambitious instruction support

Adding arithmetic too early will complicate both legality and rewriting.

### 4. Rewriting without reconstruction plans

Some conditional transforms are not just "change the type". They require a specific reconstruction pattern. The pass should capture that explicitly.

## Original MVP

The first out-of-tree implementation should:

- support only compare, select, phi, freeze, and extension/truncation patterns
- compute one legal alternative width per component
- allow that alternative width to be either narrower or wider
- solve a binary global optimization problem
- rewrite conservatively
- rely on InstCombine and DCE for cleanup

This is enough to validate the core approach before expanding to arithmetic and
richer cost models.

The current prototype has moved past this initial boundary: it now includes a
small amount of arithmetic, trunc-rooted loop rewriting, range-driven `udiv`
narrowing, local widening that reduces repeated extension paths, and a simple
global planner that already drives some whole-region widening rewrites. The
original MVP remains useful as a statement of the initial design discipline,
but it no longer describes the full implementation.

## Out-of-Tree Implementation Model

When implementation starts, the pass should use `/Users/regehr/llvm-tutor` as
the model for project structure and build integration.

The first code drop should aim to mirror the same broad shape:

- `lib/` for pass implementation
- `include/` for shared declarations if needed
- `test/` for plugin regressions
- CMake wiring that builds a normal loadable pass plugin

This is an engineering constraint rather than an algorithmic one, but it is
worth stating now so the first implementation looks like a real out-of-tree
LLVM pass rather than a one-off prototype.

## Comparison with `AggressiveInstCombine/TruncInstCombine.cpp`

This design is substantially different from LLVM's existing trunc-rooted width-reduction pass in
[`TruncInstCombine.cpp`](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp).

### 1. Rooted at `trunc` versus whole-function optimization

`TruncInstCombine` starts from an existing `trunc` and asks whether the expression feeding that trunc can be evaluated at a smaller width.

This is explicit in the file header and pass entry logic:

- [TruncInstCombine.cpp#L9](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L9)
- [TruncInstCombine.cpp#L524](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L524)

The pass designed here is not rooted at any one `trunc`. It starts from the entire integer SSA graph in a function, treats some values as anchored, and then chooses widths globally.

### 2. Closed local expression graph versus open graph with boundaries

`TruncInstCombine` only rewrites an eligible local expression graph. If it encounters unsupported instructions or incompatible outside users, it gives up.

Relevant logic:

- graph construction and early bailouts:
  [TruncInstCombine.cpp#L87](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L87)
- unsupported op bailout:
  [TruncInstCombine.cpp#L163](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L163)
- multi-user restrictions:
  [TruncInstCombine.cpp#L269](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L269)

Our design does not require a closed graph. Shared values and outside users are expected. The optimization is specifically about deciding where width boundaries should remain and where conversions should be inserted.

### 3. Single reduced type per graph versus component-wise width assignment

`TruncInstCombine` computes one reduced width for the whole expression graph and rebuilds that graph uniformly at the chosen type.

Relevant logic:

- reduced-width computation:
  [TruncInstCombine.cpp#L174](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L174)
- final chosen type:
  [TruncInstCombine.cpp#L265](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L265)
- graph rebuild:
  [TruncInstCombine.cpp#L379](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L379)

Our design instead assigns widths to width components. Different neighboring components may end up at different widths, with `zext`, `sext`, or `trunc` inserted only on cut edges where needed. Those chosen widths may be smaller or larger than the original local width.

### 4. Mainly fact-driven truncation legality versus semantic structural rules

`TruncInstCombine` uses a supported-op whitelist plus bitwidth constraints derived from the graph and some known-bits-style reasoning, especially for shifts and unsigned division/remainder.

Relevant logic:

- shift and `udiv`/`urem` handling:
  [TruncInstCombine.cpp#L298](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L298)

It does not model the class of compare-specific structural legality rules that motivate this new pass. In particular, patterns such as:

```llvm
icmp slt (sext x), (zext y)
```

can often be narrowed structurally even without separate known-bits proof. That kind of rule is central to the design here.

### 5. No support for compare-centric canonicalization versus explicit support

`TruncInstCombine` does not support `icmp` as part of its expression graph vocabulary. The supported instruction set is listed in:

- [TruncInstCombine.cpp#L48](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L48)
- [TruncInstCombine.cpp#L125](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L125)

Our design needs compare-centric reasoning from the start:

- narrowing `eq/ne`, signed, and unsigned comparisons
- handling mixed `sext`/`zext` compare patterns
- keeping min/max introduction independent from width rewriting

This is not a small extension of the existing trunc-rooted pass. It is a different transform family.

### 6. Local profitability versus global objective

`TruncInstCombine` is a local transform whose profitability rule is essentially: shrink this one graph if it is legal and does not create an obviously worse type/legalization situation.

Relevant logic:

- legal integer type checks:
  [TruncInstCombine.cpp#L241](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp#L241)

Our pass has a global objective: minimize the number of width-changing instructions across the function. That means the right decision for one region may depend on neighboring regions and anchored boundaries, and may involve either narrowing or widening a component.

### 7. Graph shrinker versus width-allocation pass

The best way to think about the difference is:

- `TruncInstCombine` is a local graph shrinker for "expression feeding trunc"
- this design is a global width-allocation pass for integer SSA regions

In this design:

- anchors define fixed-width boundaries
- width components define retargetable regions
- legality comes from both semantic rules and analysis facts
- optimization chooses widths to minimize conversion cuts

So while the existing pass is useful prior art, this proposal is not just a generalized `TruncInstCombine`. It is a different pass with a different unit of reasoning and a different objective.

## Related LLVM Passes and Analyses

As this design evolves, it should be compared not only against
`AggressiveInstCombine/TruncInstCombine.cpp`, but also against InstCombine and a
small set of other LLVM passes that already optimize or rearrange width-changing
instructions.

These fall into three categories.

### 1. Direct comparators

These are the most important existing LLVM transforms to compare against,
because they already perform IR-level width reduction or cast elimination in
some form.

#### `AggressiveInstCombine/TruncInstCombine.cpp`

- [`TruncInstCombine.cpp`](/Users/regehr/llvm-project/llvm/lib/Transforms/AggressiveInstCombine/TruncInstCombine.cpp)

This is the main AggressiveInstCombine comparator.

Its key characteristics are:

- local
- rooted at an existing `trunc`
- graph-based, but only for a closed eligible expression
- rebuilds one expression graph at one narrower width

This pass should remain the primary point of comparison on the
AggressiveInstCombine side.

#### InstCombine

InstCombine contains a substantial amount of local width optimization logic,
spread across several files.

Important parts include:

- [`InstCombineCasts.cpp#L839`](/Users/regehr/llvm-project/llvm/lib/Transforms/InstCombine/InstCombineCasts.cpp#L839)
  `narrowBinOp`: pulls `trunc` through local arithmetic and logic
- [`InstructionCombining.cpp#L2688`](/Users/regehr/llvm-project/llvm/lib/Transforms/InstCombine/InstructionCombining.cpp#L2688)
  `narrowMathIfNoOverflow`: narrows local math based on overflow checks
- [`InstCombinePHI.cpp#L806`](/Users/regehr/llvm-project/llvm/lib/Transforms/InstCombine/InstCombinePHI.cpp#L806)
  narrows PHIs composed of `zext`s and constants
- [`InstCombineCompares.cpp#L3495`](/Users/regehr/llvm-project/llvm/lib/Transforms/InstCombine/InstCombineCompares.cpp#L3495)
  performs some narrow-compare elimination
- [`InstCombineSimplifyDemanded.cpp`](/Users/regehr/llvm-project/llvm/lib/Transforms/InstCombine/InstCombineSimplifyDemanded.cpp)
  contains demanded-bits-driven local narrowing and cast simplification

InstCombine is still fundamentally local and pattern-based, but it already
contains many of the ingredients our pass will need to distinguish itself from:

- compare narrowing
- cast sinking/hoisting
- narrow arithmetic under proof obligations
- localized PHI shrinking

But InstCombine is still fundamentally a local rewrite system. It does not
solve the global width-assignment problem where widening one region may reduce
the total number of conversions elsewhere.

Going forward, our pass should be compared against InstCombine as a whole, not
just against one file.

### 2. Adjacent IR-level prior art

These passes do not solve the same global optimization problem, but they
directly manipulate width-changing instructions or narrow computations in
semantically important ways.

#### `CorrelatedValuePropagation`

- [`CorrelatedValuePropagation.cpp#L775`](/Users/regehr/llvm-project/llvm/lib/Transforms/Scalar/CorrelatedValuePropagation.cpp#L775)
  narrows `sdiv` and `srem` using constant-range reasoning
- [`CorrelatedValuePropagation.cpp#L903`](/Users/regehr/llvm-project/llvm/lib/Transforms/Scalar/CorrelatedValuePropagation.cpp#L903)
  narrows `udiv` and `urem` using constant-range reasoning

This is especially relevant because it already does:

- range-driven legality reasoning
- local insertion of `trunc + op + ext`

This makes it strong prior art for the legality side of the design.

#### `BDCE`

- [`BDCE.cpp#L9`](/Users/regehr/llvm-project/llvm/lib/Transforms/Scalar/BDCE.cpp#L9)

BDCE is not a width-minimization pass, but it tracks demanded bits and:

- removes dead bit computations
- converts `sext` to `zext` when sign-extension bits are not demanded

This is useful adjacent prior art because it demonstrates a real IR transform
driven by demanded-bits information.

#### `CodeGenPrepare`

Important sections include:

- [`CodeGenPrepare.cpp#L2356`](/Users/regehr/llvm-project/llvm/lib/CodeGen/CodeGenPrepare.cpp#L2356)
  sinks shift+trunc patterns for target matching
- [`CodeGenPrepare.cpp#L4899`](/Users/regehr/llvm-project/llvm/lib/CodeGen/CodeGenPrepare.cpp#L4899)
  simplifies `ext(trunc(x))` and related extension/truncation chains
- [`CodeGenPrepare.cpp#L7140`](/Users/regehr/llvm-project/llvm/lib/CodeGen/CodeGenPrepare.cpp#L7140)
  promotes or repositions extends to form extloads and improve codegen

This is not a direct comparator because it is backend-oriented, but it is very
relevant for two reasons:

- it contains a lot of practical cast-motion logic
- it provides examples of target-sensitive profitability reasoning

### 3. Backend prior art

These do not operate at the same IR level or have the same objective, but they
are still worth studying for legality rules, cost models, and narrowing
machinery.

#### SelectionDAG DAGCombiner

- [`DAGCombiner.cpp#L21785`](/Users/regehr/llvm-project/llvm/lib/CodeGen/SelectionDAG/DAGCombiner.cpp#L21785)
  narrows load/op/store sequences

There are many other narrowing folds in `DAGCombiner.cpp`, especially around:

- extloads
- truncstores
- narrow loads
- narrow vector operations

This is useful backend prior art for profitability and memory-width narrowing.

#### GlobalISel Legalizer

- [`LegalizerHelper.cpp`](/Users/regehr/llvm-project/llvm/lib/CodeGen/GlobalISel/LegalizerHelper.cpp)

GlobalISel contains extensive `narrowScalar*` machinery. It is solving a
legalization problem rather than an optimization problem, but it is still worth
reading for:

- narrow reconstruction recipes
- extension/truncation boundary handling
- operation-specific legality structure

## Supporting Analyses

Some LLVM components are not direct transform comparators, but they are likely
to be important inputs to the new pass.

### `DemandedBits`

- [`DemandedBits.cpp#L9`](/Users/regehr/llvm-project/llvm/lib/Analysis/DemandedBits.cpp#L9)

This analysis is central for proving that some high bits do not matter.

### `ValueTracking`

- [`ValueTracking.cpp`](/Users/regehr/llvm-project/llvm/lib/Analysis/ValueTracking.cpp)

This is the main source for:

- known bits
- sign-bit reasoning
- related value facts

### `CmpInstAnalysis`

- [`CmpInstAnalysis.cpp#L165`](/Users/regehr/llvm-project/llvm/lib/Analysis/CmpInstAnalysis.cpp#L165)

This includes useful compare decomposition logic, such as reasoning through
`trunc` in `eq/ne` bit-test style patterns.

## Practical Conclusion

The main passes to compare against as this work proceeds are:

1. `AggressiveInstCombine/TruncInstCombine`
2. InstCombine
3. `CorrelatedValuePropagation`
4. `BDCE`
5. `CodeGenPrepare`

The backend components `DAGCombiner` and `GlobalISel` should be treated as
secondary prior art rather than direct competitors.

## Future Extensions

Once the framework is working, possible extensions include:

- support for arithmetic operations
- richer candidate width sets
- approximate multi-label optimization
- profile-guided or target-aware cost models
- interaction with min/max canonicalization
- better integration with existing LLVM demanded-bits and known-bits reasoning

## Summary

The proposed design is:

- use width components to compress the problem
- use a conservative legality lattice with `MinAny`, `MinZ`, and `MinS`
- combine structural semantic rules with known-bits/range facts
- choose widths globally with a binary cut formulation
- allow the chosen alternative width to be either narrower or wider when that
  reduces total conversion count
- keep the first implementation narrow in scope

This should produce a pass that is simple enough to prototype out of tree, while still being strong enough to capture interesting transformations that are not purely local.
