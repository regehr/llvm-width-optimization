; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; A truncate is pulled through an add when one operand is already narrow via zext.

define i8 @f(i8 %x, i32 %y) {
  %ex = zext i8 %x to i32
  %a = add i32 %ex, %y
  %t = trunc i32 %a to i8
  ret i8 %t
}
