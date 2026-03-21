; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): NO
; A signed predicate against a zero-extended value is NOT narrowed: signed
; comparison of a sext-produced value has different semantics than unsigned.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @slt_zext_const(i8 %x) {
  %ext = zext i8 %x to i32
  %cmp = icmp slt i32 %ext, 100
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @slt_zext_const(
; CHECK: %ext = zext i8 %x to i32
; CHECK: %cmp = icmp slt i32 %ext, 100
