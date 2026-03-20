; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Assumptions let InstCombine compare a truncated value against a zext source at i32.

declare void @llvm.assume(i1)

define i1 @f(i32 %x, i8 %y) {
entry:
  %x_lb_only = icmp ult i32 %x, 65536
  call void @llvm.assume(i1 %x_lb_only)
  %x16 = trunc i32 %x to i16
  %y16 = zext i8 %y to i16
  %r = icmp ugt i16 %x16, %y16
  ret i1 %r
}
