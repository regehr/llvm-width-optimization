; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Range information from llvm.assume lets LLVM narrow the udiv to i8.

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
