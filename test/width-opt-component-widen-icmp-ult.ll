; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @f(i1 %c, i8 %x, i8 %y) {
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
  %cmp = icmp ult i8 %p, %y
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @f(
; CHECK: merge:
; CHECK: %[[P:.*]] = phi i32 [ %{{.*}}, %left ], [ 7, %right ]
; CHECK-NOT: zext i8 %p to i32
; CHECK: %[[Y32:.*]] = zext i8 %y to i32
; CHECK: %[[CMP:.*]] = icmp ult i32 %[[P]], %[[Y32]]
; CHECK: ret i1 %[[CMP]]
