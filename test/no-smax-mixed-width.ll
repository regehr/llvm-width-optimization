; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): NO
; LLVM does not currently combine the width change and the smax introduction here.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i16 @f(i16 %x, i8 %y) {
  %x32 = sext i16 %x to i32
  %y32 = zext i8 %y to i32
  %c = icmp slt i32 %x32, %y32
  %y16 = zext i8 %y to i16
  %r = select i1 %c, i16 %y16, i16 %x
  ret i16 %r
}

; CHECK-LABEL: define i16 @f(
; CHECK: %[[Y16:.*]] = zext i8 %y to i16
; CHECK: %[[R:.*]] = call i16 @llvm.smax.i16(i16 %x, i16 %[[Y16]])
; CHECK: ret i16 %[[R]]
