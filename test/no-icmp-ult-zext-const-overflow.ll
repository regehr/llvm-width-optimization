; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): NO
; A constant that does not fit in the extension source width is not narrowed.
; 300 does not round-trip through i8: trunc(300) = 44, zext(44) = 44 != 300.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @ult_zext_large_const(i8 %x) {
  %ext = zext i8 %x to i32
  %cmp = icmp ult i32 %ext, 300
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @ult_zext_large_const(
; CHECK: %ext = zext i8 %x to i32
; CHECK: %cmp = icmp ult i32 %ext, 300
