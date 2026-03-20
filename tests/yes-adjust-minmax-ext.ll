; Current LLVM (/Users/regehr/llvm-project/for-alive/bin/opt -passes='default<O2>' -S): YES
; InstCombine adjusts ext(select(cmp)) into a narrower min/max and widens only once.

define i64 @f(i32 %a) {
entry:
  %a_ext = sext i32 %a to i64
  %cmp = icmp sgt i32 %a, -1
  %max = select i1 %cmp, i64 %a_ext, i64 0
  ret i64 %max
}
