; A trunc-vs-narrow icmp may be widened only when any new zext is paid for by
; removable boundary instructions. If the trunc stays live for another user,
; leave the compare alone instead of adding a new zext on the other side.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @shared_trunc_not_widened(i8 %n) {
entry:
  %wide = zext i8 %n to i64
  %tr = trunc i64 %wide to i8
  %div = sdiv i8 -1, %tr
  %rem = srem i8 -1, %div
  %cmp = icmp ult i8 %tr, %rem
  ret i1 %cmp
}

; CHECK-LABEL: define i1 @shared_trunc_not_widened(
; CHECK: %tr = trunc i64 %wide to i8
; CHECK: %div = sdiv i8 -1, %tr
; CHECK: %rem = srem i8 -1, %div
; CHECK: %cmp = icmp ult i8 %tr, %rem
; CHECK-NOT: icmp ult i64
; CHECK-NOT: zext i8 %rem to i64
