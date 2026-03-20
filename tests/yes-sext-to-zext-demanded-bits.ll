; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; The sign extension is weakened to zext because only low bits are demanded.

define i32 @f(i8 %x) {
  %sx = sext i8 %x to i32
  %r = and i32 %sx, 255
  ret i32 %r
}
