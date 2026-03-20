; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; Demanded bits can change a shared sext into a zext even with multiple uses.

define i32 @f(i16 %a) {
entry:
  %ext = sext i16 %a to i32
  %and = and i32 %ext, 65280
  %lsr = lshr i32 %ext, 8
  %and2 = and i32 %lsr, 255
  %or = or i32 %and, %and2
  ret i32 %or
}
