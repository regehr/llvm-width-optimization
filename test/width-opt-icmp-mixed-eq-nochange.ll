; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @f(i16 %x, i8 %y) {
entry:
  %x32 = sext i16 %x to i32
  %y32 = zext i8 %y to i32
  %c = icmp eq i32 %x32, %y32
  ret i1 %c
}

; CHECK-LABEL: define i1 @f(
; CHECK: %x32 = sext i16 %x to i32
; CHECK: %y32 = zext i8 %y to i32
; CHECK: %c = icmp eq i32 %x32, %y32
; CHECK: ret i1 %c
