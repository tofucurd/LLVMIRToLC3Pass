@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @LC3_main() {
  %arr = alloca [5 x i32]
  %p0 = getelementptr [5 x i32], ptr %arr, i32 0, i32 0
  store i32 10, ptr %p0
  %p3 = getelementptr [5 x i32], ptr %arr, i32 0, i32 3
  store i32 42, ptr %p3
  %v = load i32, ptr %p3                ; 42
  %sum = add i32 %v, 1                  ; 43
  store i32 %sum, ptr %p0
  %v2 = load i32, ptr %p0               ; 43
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %v2)
  ret void
}
