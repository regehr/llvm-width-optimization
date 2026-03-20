; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; InstCombine turns zext(trunc(x)) into a mask at an intermediate width.

define i32 @f(i64 %x) {
entry:
  %t = trunc i64 %x to i16
  %e = zext i16 %t to i32
  ret i32 %e
}
