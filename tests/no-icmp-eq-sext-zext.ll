; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): NO
; Equality compare across mixed sign/zero extensions is not narrowed.

define i1 @f(i16 %x, i8 %y) {
  %x32 = sext i16 %x to i32
  %y32 = zext i8 %y to i32
  %c = icmp eq i32 %x32, %y32
  ret i1 %c
}
