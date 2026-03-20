; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='print<width-components>' -disable-output %s 2>&1 | FileCheck %s

declare i16 @g(i16)

define i16 @f(i16, ptr %p, ptr %fp) {
entry:
  %0 = load i16, ptr %p, align 2
  %1 = ptrtoint ptr %p to i64
  %2 = trunc i64 %1 to i16
  %3 = call i16 @g(i16 %0)
  %4 = load ptr, ptr %fp, align 8
  %5 = call i16 %4(i16 %0)
  %6 = add i16 %0, %3
  ret i16 %6
}

; CHECK: Width components for function 'f':
; CHECK: [arg]
; CHECK: [anchor]
; CHECK: [conditional]
