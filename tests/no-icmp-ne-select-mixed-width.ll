; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): NO
; LLVM canonicalizes the predicate, but it does not narrow the mixed-width compare/select.

define i16 @f(i16 %x, i8 %y) {
  %x32 = sext i16 %x to i32
  %y32 = zext i8 %y to i32
  %c = icmp ne i32 %x32, %y32
  %y16 = zext i8 %y to i16
  %r = select i1 %c, i16 %y16, i16 %x
  ret i16 %r
}
