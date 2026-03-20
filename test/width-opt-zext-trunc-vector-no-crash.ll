; Vector zext(trunc(x)) patterns are outside the current scalar mask-folding
; logic. The pass should leave them alone rather than crashing.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define <2 x i65> @foo(<2 x i64> %t) {
entry:
  %a = trunc <2 x i64> %t to <2 x i32>
  %b = zext <2 x i32> %a to <2 x i65>
  ret <2 x i65> %b
}

; CHECK-LABEL: define <2 x i65> @foo(
; CHECK: %[[A:.*]] = trunc <2 x i64> %t to <2 x i32>
; CHECK: %[[B:.*]] = zext <2 x i32> %[[A]] to <2 x i65>
; CHECK: ret <2 x i65> %[[B]]
