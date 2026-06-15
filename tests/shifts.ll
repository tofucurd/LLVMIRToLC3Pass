@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @LC3_main() {
  %a = shl i32 1, 4         ; 16
  %b = lshr i32 128, 3      ; 16
  %c = ashr i32 -64, 2      ; -16
  %d = shl i32 %a, 1        ; 32
  %e = lshr i32 256, %a     ; 256/65536=0 (256>>16=0)
  %f = ashr i32 -256, %b    ; -256>>16=-1
  %sum = add i32 %d, %f      ; 32 + (-1) = 31
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %sum)
  ret void
}
