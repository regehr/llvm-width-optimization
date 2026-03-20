; Narrowing a trunc-rooted add is only profitable when removable boundary
; instructions pay for any new operand truncs. If both add operands are still
; wide values, leave the trunc of add in place.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @shared_wide_add_operands_not_narrowed(i8 %a, i8 %c, i8 %d) {
entry:
  %a32 = zext i8 %a to i32
  %c32 = zext i8 %c to i32
  %d32 = zext i8 %d to i32
  %sub = xor i32 255, %c32
  %mul1 = mul i32 %a32, %sub
  %mul2 = mul i32 %c32, %d32
  %add = add i32 %mul1, %mul2
  %trunc = trunc i32 %add to i16
  %cmp = icmp eq i16 %trunc, 1234
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @shared_wide_add_operands_not_narrowed(
; CHECK: %add = add i32 %mul1, %mul2
; CHECK: %trunc = trunc i32 %add to i16
; CHECK: %cmp = icmp eq i16 %trunc, 1234
; CHECK-NOT: trunc i32 %mul1 to i16
; CHECK-NOT: trunc i32 %mul2 to i16
