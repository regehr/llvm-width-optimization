; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

; A shift amount >= the target width would make the narrow lshr poison, so
; this must not be transformed.

define i8 @lshr_shift_too_large(i8 %x) {
  %wide = zext i8 %x to i32
  %shifted = lshr i32 %wide, 8
  %narrow = trunc i32 %shifted to i8
  ret i8 %narrow
}

; CHECK-LABEL: define i8 @lshr_shift_too_large(
; CHECK: zext
; CHECK: lshr
; CHECK: trunc
