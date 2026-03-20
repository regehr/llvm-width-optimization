; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @f(i8 %x, i8 %y) {
entry:
  %wide = zext i8 %x to i32
  %tx = trunc i32 %wide to i16
  %sy = sext i8 %y to i16
  %cmp = icmp ult i16 %tx, %sy
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @f(
; CHECK: %wide = zext i8 %x to i32
; CHECK: %sy = sext i8 %y to i16
; CHECK: %[[WIDEY:.*]] = zext i16 %sy to i32
; CHECK: %cmp = icmp ult i32 %wide, %[[WIDEY]]
; CHECK: ret i1 %cmp
