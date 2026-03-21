; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

; A loop-carried add recurrence truncated at exit.  The init and step are both
; zero-extended from i8, so the whole recurrence can run at i8 width.

define i8 @add_loop(i8 %init, i8 %step, i32 %n) {
entry:
  %init32 = zext i8 %init to i32
  %step32 = zext i8 %step to i32
  br label %loop
loop:
  %p = phi i32 [ %init32, %entry ], [ %next, %loop ]
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %next = add i32 %p, %step32
  %inc = add i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %loop
exit:
  %t = trunc i32 %next to i8
  ret i8 %t
}

; CHECK-LABEL: define i8 @add_loop(
; CHECK: %[[PN:.*]] = phi i8 [ %init, %entry ], [ %[[ADD:.*]], %loop ]
; CHECK: %[[ADD]] = add i8 %[[PN]], %step
; CHECK-NOT: zext
; CHECK-NOT: trunc
; CHECK: ret i8 %[[ADD]]

; Constant step variant.

define i8 @add_loop_const_step(i8 %init, i32 %n) {
entry:
  %init32 = zext i8 %init to i32
  br label %loop
loop:
  %p = phi i32 [ %init32, %entry ], [ %next, %loop ]
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %next = add i32 %p, 3
  %inc = add i32 %i, 1
  %done = icmp eq i32 %inc, %n
  br i1 %done, label %exit, label %loop
exit:
  %t = trunc i32 %next to i8
  ret i8 %t
}

; CHECK-LABEL: define i8 @add_loop_const_step(
; CHECK: %[[PN:.*]] = phi i8 [ %init, %entry ], [ %[[ADD:.*]], %loop ]
; CHECK: %[[ADD]] = add i8 %[[PN]], 3
; CHECK-NOT: zext
; CHECK-NOT: trunc
; CHECK: ret i8 %[[ADD]]
