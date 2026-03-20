; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i32 @f(i1 %c, i8 %x, i8 %y) {
entry:
  br i1 %c, label %left, label %right

left:
  br label %merge

right:
  br label %merge

merge:
  %p = phi i8 [ %x, %left ], [ 7, %right ]
  %a = zext i8 %p to i32
  %b = zext i8 %p to i32
  %d = zext i8 %p to i32
  %e = zext i8 %p to i32
  %cmp = icmp ult i8 %p, %y
  %sum0 = add i32 %a, %b
  %sum1 = add i32 %d, %e
  %sum = add i32 %sum0, %sum1
  %sel = select i1 %cmp, i32 %sum, i32 0
  ret i32 %sel
}

; CHECK-LABEL: define i32 @f(
; CHECK: merge:
; CHECK: %[[P:.*]] = phi i32 [ %{{.*}}, %left ], [ 7, %right ]
; CHECK-NOT: zext i8 %p to i32
; CHECK: %[[Y32:.*]] = zext i8 %y to i32
; CHECK: %[[CMP:.*]] = icmp ult i32 %[[P]], %[[Y32]]
; CHECK: %[[SUM0:.*]] = add i32 %[[P]], %[[P]]
; CHECK: %[[SUM1:.*]] = add i32 %[[P]], %[[P]]
; CHECK: %[[SUM:.*]] = add i32 %[[SUM0]], %[[SUM1]]
; CHECK: %[[SEL:.*]] = select i1 %[[CMP]], i32 %[[SUM]], i32 0
; CHECK: ret i32 %[[SEL]]
