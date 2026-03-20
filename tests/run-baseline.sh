#!/bin/zsh
set -euo pipefail

OPT_BIN="${OPT_BIN:-/Users/regehr/llvm-project/for-alive/bin/opt}"
TEST_DIR="${0:A:h}"

if [[ ! -x "${OPT_BIN}" ]]; then
  echo "error: opt binary not found: ${OPT_BIN}" >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "${tmpdir}"' EXIT

require_pattern() {
  local file="$1"
  local pattern="$2"
  if ! rg -q --fixed-strings -- "${pattern}" "${file}"; then
    echo "missing required pattern: ${pattern}" >&2
    return 1
  fi
}

forbid_pattern() {
  local file="$1"
  local pattern="$2"
  if rg -q --fixed-strings -- "${pattern}" "${file}"; then
    echo "found forbidden pattern: ${pattern}" >&2
    return 1
  fi
}

check_output() {
  local base="$1"
  local out="$2"

  case "${base}" in
    no-branch-almost-nonnegative-sext-to-zext.ll)
      require_pattern "${out}" "sext i32 %n to i64"
      ;;
    no-global-widening-two-wide-widths.ll)
      require_pattern "${out}" "zext i8 %a to i32"
      require_pattern "${out}" "zext i8 %a to i64"
      ;;
    no-icmp-eq-sext-zext.ll)
      require_pattern "${out}" "icmp eq i32 %x32, %y32"
      ;;
    no-icmp-ne-select-mixed-width.ll)
      require_pattern "${out}" "icmp eq i32 %x32, %y32"
      require_pattern "${out}" "select i1 %c.not, i16 %x, i16 %y16"
      ;;
    no-icmp-slt-sext-zext.ll)
      require_pattern "${out}" "icmp slt i32 %x32, %y32"
      ;;
    no-phi-sexts-constant.ll)
      require_pattern "${out}" "phi i32"
      ;;
    no-smax-mixed-width.ll)
      require_pattern "${out}" "select i1 %c, i16 %y16, i16 %x"
      forbid_pattern "${out}" "@llvm.smax"
      ;;
    yes-adjust-minmax-ext.ll)
      require_pattern "${out}" "@llvm.smax.i32"
      require_pattern "${out}" "zext nneg i32 %narrow to i64"
      ;;
    yes-branch-nonnegative-sext-to-zext.ll)
      require_pattern "${out}" "zext nneg i32 %n to i64"
      ;;
    yes-freeze-icmp-ult-zext-zext.ll)
      require_pattern "${out}" "freeze i8 %x"
      require_pattern "${out}" "icmp ugt i16 %y.fr, %1"
      ;;
    yes-icmp-assume-trunc-trunc-eq.ll)
      require_pattern "${out}" "icmp eq i32 %x, %y"
      forbid_pattern "${out}" "trunc i32 %x to i16"
      ;;
    yes-icmp-assume-trunc-zext.ll)
      require_pattern "${out}" "zext i8 %y to i32"
      require_pattern "${out}" "icmp samesign ugt i32 %x, %0"
      ;;
    yes-icmp-eq-zext-zext.ll)
      require_pattern "${out}" "icmp eq i16 %y, %1"
      ;;
    yes-icmp-slt-sext-sext.ll)
      require_pattern "${out}" "icmp sgt i16 %y, %1"
      ;;
    yes-icmp-ult-zext-zext.ll)
      require_pattern "${out}" "icmp ugt i16 %y, %1"
      ;;
    yes-phi-zexts-constant.ll)
      require_pattern "${out}" "phi i8"
      require_pattern "${out}" "zext i8 %p.shrunk to i32"
      ;;
    yes-select-sext-trunc.ll)
      require_pattern "${out}" "sext i8 %a to i16"
      require_pattern "${out}" "select i1 %cond, i16 %sub, i16 %conv"
      ;;
    yes-sext-multiuse-to-zext.ll)
      require_pattern "${out}" "zext i16 %a to i32"
      ;;
    yes-sext-to-zext-demanded-bits.ll)
      require_pattern "${out}" "zext i8 %x to i32"
      ;;
    yes-smax-same-width.ll)
      require_pattern "${out}" "@llvm.smax.i16"
      ;;
    yes-trunc-add-zext-operand.ll)
      require_pattern "${out}" "trunc i32 %y to i8"
      require_pattern "${out}" "add i8 %x, %1"
      ;;
    yes-trunc-phi-loop.ll)
      require_pattern "${out}" "zext i8 %x to i16"
      require_pattern "${out}" "shl i16 %zext, 10"
      ;;
    yes-udiv-range-narrowing.ll)
      require_pattern "${out}" "udiv i8 %d.lhs.trunc, %d.rhs.trunc"
      require_pattern "${out}" "zext i8 %d1 to i32"
      ;;
    yes-umax-same-width.ll)
      require_pattern "${out}" "@llvm.umax.i16"
      ;;
    yes-zext-trunc-to-mask.ll)
      require_pattern "${out}" "trunc i64 %x to i32"
      require_pattern "${out}" "and i32 %0, 65535"
      ;;
    *)
      echo "no baseline rule for ${base}" >&2
      return 1
      ;;
  esac
}

failures=0

for f in "${TEST_DIR}"/*.ll; do
  base="$(basename "${f}")"
  first_line="$(sed -n '1p' "${f}")"
  if [[ "${first_line}" != *": YES" && "${first_line}" != *": NO" ]]; then
    echo "FAIL ${base}: missing YES/NO header" >&2
    failures=$((failures + 1))
    continue
  fi

  out="${tmpdir}/${base}.out"
  if ! "${OPT_BIN}" -passes='default<O2>' -S "${f}" > "${out}"; then
    echo "FAIL ${base}: opt failed" >&2
    failures=$((failures + 1))
    continue
  fi

  if ! check_output "${base}" "${out}"; then
    echo "FAIL ${base}: output did not match baseline expectation" >&2
    failures=$((failures + 1))
    continue
  fi

  echo "PASS ${base}"
done

if [[ "${failures}" -ne 0 ]]; then
  echo
  echo "${failures} test(s) failed" >&2
  exit 1
fi

echo
echo "All baseline tests passed."
