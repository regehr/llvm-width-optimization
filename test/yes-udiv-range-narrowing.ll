; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Range information from llvm.assume lets the udiv execute at i8.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i32 @f(i32 %x, i32 %y) {
entry:
  %cx = icmp ult i32 %x, 256
  call void @llvm.assume(i1 %cx)
  %cy = icmp ult i32 %y, 256
  call void @llvm.assume(i1 %cy)
  %d = udiv i32 %x, %y
  ret i32 %d
}

declare void @llvm.assume(i1)

; CHECK-LABEL: define i32 @f(
; CHECK: %cx = icmp ult i32 %x, 256
; CHECK: call void @llvm.assume(i1 %cx)
; CHECK: %cy = icmp ult i32 %y, 256
; CHECK: call void @llvm.assume(i1 %cy)
; CHECK: %[[X8:.*]] = trunc i32 %x to i8
; CHECK: %[[Y8:.*]] = trunc i32 %y to i8
; CHECK: %[[D8:.*]] = udiv i8 %[[X8]], %[[Y8]]
; CHECK: %[[D32:.*]] = zext i8 %[[D8]] to i32
; CHECK-NOT: udiv i32
; CHECK: ret i32 %[[D32]]
