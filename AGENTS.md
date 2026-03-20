# AGENTS.md

This repository expects agents to work in a review-driven, proof-driven way.
Follow the rules below unless the user explicitly overrides them.

## Context First

- Before reviewing or changing `lib/WidthOpt.cpp`, read the markdown docs first:
  - `README.md`
  - `width-minimization-design.md`
- Use the docs to understand current scope, policy boundaries, and which
  transforms are already intended to exist.

## Review Scope

- When asked to review effectiveness, focus on weaknesses or flaws in the
  existing logic that cause the pass to remove fewer width changes than it
  should.
- Do not pad the review with speculative new features that the pass does not
  currently aim to handle. Keep the distinction clear:
  - existing logic that is weak, inconsistent, or needlessly conservative
  - new functionality that would broaden scope
- If you identify a correctness concern while reviewing effectiveness, call it
  out separately.

## README TODO Discipline

- Concrete review findings should be reflected in `README.md` as actionable
  TODOs with enough detail to preserve the technical point.
- Keep the TODO list current:
  - add new concrete items when they are discovered
  - remove items once they are completed
- Do not leave completed work in the active TODO list.

## Implementation Priorities

- Prefer high-priority, self-contained items that improve effectiveness within
  the current supported scope.
- Make progress proactively. Do not wait for the user to remind you to update
  TODOs, add tests, or run broader verification.

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
- Run Alive2 when it is usable.
- If Alive2 fails because of an `alive-tv` crash, distinguish tool failure from
  pass failure before drawing conclusions.

## Alive2 Crash Handling

- If `alive-tv` crashes, reduce the issue to a small standalone source/target
  IR pair.
- Store reduced reproducers under `repro/` rather than `test/`, so they do not
  get picked up by normal lit or corpus verification automatically.
- After the user rebuilds Alive2, rerun the reduced repro to see whether the
  crash still reproduces.

## Useful Commands

- Configure/build plugin:
  - `cmake -S /Users/regehr/llvm-width-optimization -B /tmp/llvm-width-optimization-build-fixpoint -DWO_LLVM_INSTALL_DIR=/Users/regehr/llvm-project/for-alive`
  - `cmake --build /tmp/llvm-width-optimization-build-fixpoint -j4`
- Run full lit:
  - `/Users/regehr/llvm-project/for-alive/bin/llvm-lit -sv /tmp/llvm-width-optimization-build-fixpoint/test`
- Run Alive2 script with a specific plugin:
  - `python3 /Users/regehr/llvm-width-optimization/scripts/verify_with_alive2.py --plugin /tmp/llvm-width-optimization-build-fixpoint/lib/libWidthOpt.dylib`
