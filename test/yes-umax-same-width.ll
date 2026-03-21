; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Same-width unsigned compare/select canonicalizes to llvm.umax.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i16 @f(i16 %x, i16 %y) {
  %c = icmp ult i16 %x, %y
  %r = select i1 %c, i16 %y, i16 %x
  ret i16 %r
}

; CHECK-LABEL: define i16 @f(
; CHECK-NOT: icmp
; CHECK-NOT: select
; CHECK: %[[R:.*]] = call i16 @llvm.umax.i16(i16 %x, i16 %y)
; CHECK: ret i16 %[[R]]
