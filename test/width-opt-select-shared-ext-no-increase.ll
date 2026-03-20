; A select-of-exts rewrite should be driven by removable extension
; instructions. If the surviving wide uses would require adding a new result
; extension but the arm extension is still shared, leave the select alone.
; RUN: opt -load-pass-plugin %shlibdir/libWidthOpt%shlibext \
; RUN:   -passes='width-opt' -S %s | FileCheck %s

define i1 @select_no_increase_shared_zext(i2 %arg0, i2 %arg1, i1 %arg2, i2 %arg3) {
entry:
  %cmp0 = icmp sgt i2 0, %arg0
  %zext = zext i1 %arg2 to i2
  %sel0 = select i1 %cmp0, i2 0, i2 %zext
  %trunc = trunc i2 %sel0 to i1
  %sel1 = select i1 %trunc, i2 %arg1, i2 0
  %cmp1 = icmp sle i1 %cmp0, %trunc
  %sel2 = select i1 %cmp1, i2 0, i2 %arg1
  %and = and i2 %zext, %sel2
  %sel3 = select i1 %arg2, i2 0, i2 %arg3
  %div = sdiv i2 1, %sel3
  %cmp2 = icmp uge i2 %sel1, %and
  %sext = sext i1 %cmp2 to i2
  %cmp3 = icmp sgt i2 %div, %sext
  %cmp4 = icmp sgt i2 %sel0, 0
  %lshr = lshr i1 %cmp3, %cmp4
  ret i1 %lshr
}

; CHECK-LABEL: define i1 @select_no_increase_shared_zext(
; CHECK: %zext = zext i1 %arg2 to i2
; CHECK: %sel0 = select i1 %cmp0, i2 0, i2 %zext
; CHECK: %trunc = trunc i2 %sel0 to i1
; CHECK: %cmp4 = icmp sgt i2 %sel0, 0
; CHECK-NOT: select i1 %cmp0, i1 false, i1 %arg2
