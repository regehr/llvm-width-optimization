  define i16 @src(i16 %x, i8 %y) {
    %x32 = sext i16 %x to i32
    %y32 = zext i8 %y to i32
    %c = icmp slt i32 %x32, %y32
    %y16 = zext i8 %y to i16
    %r = select i1 %c, i16 %y16, i16 %x
    ret i16 %r
  }

  define i16 @tgt(i16 %x, i8 %y) {
    %y16 = zext i8 %y to i16
    %r = call i16 @llvm.smax.i16(i16 %x, i16 %y16)
    ret i16 %r
  }
