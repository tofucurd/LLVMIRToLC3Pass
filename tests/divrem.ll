@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @LC3_main() {
  %a = udiv i32 100, 7      ; 14
  %b = urem i32 100, 7      ; 2
  %c = lshr i32 64, 2       ; 16
  %sum = add i32 %a, %b
  %sum2 = add i32 %sum, %c   ; 14+2+16 = 32
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %sum2)
  ret void
}
