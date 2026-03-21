# AGENTS.md

This repository expects agents to work in a review-driven, proof-driven way.
Follow the rules below unless the user explicitly overrides them.

## Context First

- Before reviewing or changing `lib/WidthOpt.cpp`, read the markdown docs first:
  - `README.md`
  - `width-minimization-design.md`
- Use the docs to understand current scope, policy boundaries, and which
  transforms are already intended to exist.

## High-Level Agent Workflow

Often, you will be asked to improve this width optimizer. Here's how
to do it.
1. Create an LLVM IR file that contains width instructions that can be
   optimized away, but that the current pass cannot handle. If you cannot
   create such a file, then say so and ask for help from the user.
2. Verify that the pass cannot perform the desired transformations by
   running the pass.
3. Verify that the desired transformation is a refinement using `alive-tv`.
4. Fix WidthOpt.cpp to handle the new example.
5. Add one or more unit tests based on the example.

## README TODO Discipline

- Concrete review findings should be reflected in `README.md` as actionable
  TODOs with enough detail to preserve the technical point.
- Keep the TODO list current:
  - add new concrete items when they are discovered
  - remove items once they are completed
- Do not leave completed work in the active TODO list.

## Mandatory Scope Boundary

- This is mandatory, not optional advice: the job of this pass is to remove
  `sext`, `zext`, and `trunc`.
- Do not treat running unrelated general cleanup or canonicalization passes as
  a valid way to make `width-opt` look better.
- In particular, do not solve effectiveness gaps by appending or relying on
  `InstSimplify`, `InstCombine`, `AggressiveInstCombine`, or other generic
  optimization passes after `width-opt`.
- Those passes may be used only as debugging aids or comparison baselines when
  analyzing a missed optimization. They are not part of the solution.

## Tests For Effectiveness Work

- Every effectiveness improvement must come with a regression test.
- The test should cover a case that was handled badly before the patch and is
  handled well after the patch.
- Do not assume that the case failed before. Check it against the pre-patch
  plugin or otherwise verify the before/after behavior directly.
- When existing tests now optimize further because of the patch, update their
  expectations deliberately rather than treating this as incidental fallout.

## Verification

- After implementation, run focused tests for the changed area first.
- Then run the full lit suite for the patched build.
- Run Alive2 to check correctness of the transformation

## Alive2 Crash Handling

- If `alive-tv` crashes, reduce the issue to a small standalone source/target
  IR pair.
- Store reduced reproducers under `repro/` rather than `test/`, so they do not
  get picked up by normal lit or corpus verification automatically.

## Useful Commands

- Configure/build plugin:
  - `cmake -S /Users/regehr/llvm-width-optimization -B /tmp/llvm-width-optimization-build-fixpoint -DWO_LLVM_INSTALL_DIR=/Users/regehr/llvm-project/for-alive`
  - `cmake --build /tmp/llvm-width-optimization-build-fixpoint -j4`
- Run the single CMake test target:
  - `cmake --build /tmp/llvm-width-optimization-build-fixpoint --target check`
- Run full lit:
  - `/Users/regehr/llvm-project/for-alive/bin/llvm-lit -sv /tmp/llvm-width-optimization-build-fixpoint/test`
- Run Alive2 script with a specific plugin:
  - `python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py --plugin /tmp/llvm-width-optimization-build-fixpoint/lib/libWidthOpt.dylib`
