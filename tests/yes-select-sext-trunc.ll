; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; A select over sext-derived arithmetic is narrowed before the final trunc.

define i16 @f(i8 %a, i1 %cond) {
entry:
  %conv = sext i8 %a to i32
  %sub = sub nsw i32 0, %conv
  %sel = select i1 %cond, i32 %sub, i32 %conv
  %t = trunc i32 %sel to i16
  ret i16 %t
}
