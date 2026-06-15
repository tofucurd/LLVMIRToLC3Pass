@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @LC3_main() {
  %a = add i32 3, 4
  %b = sub i32 10, %a
  %c = mul i32 %a, %b
  %d = and i32 %c, 15
  %e = or i32 %d, 1
  %f = shl i32 %e, 2
  ; expect: 3+4=7, 10-7=3, 7*3=21, 21&15=5, 5|1=5, 5<<2=20
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %f)
  ret void
}
