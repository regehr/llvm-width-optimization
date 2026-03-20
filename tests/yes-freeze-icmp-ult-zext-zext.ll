; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Freeze does not block narrowing here; LLVM freezes the narrow values instead.

define i1 @f(i8 %x, i16 %y) {
  %x32 = zext i8 %x to i32
  %y32 = zext i16 %y to i32
  %fx = freeze i32 %x32
  %fy = freeze i32 %y32
  %c = icmp ult i32 %fx, %fy
  ret i1 %c
}
