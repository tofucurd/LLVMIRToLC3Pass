@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"
@g = global i32 42
@arr = global [3 x i32] [i32 10, i32 20, i32 30]
@zarr = global [4 x i32] zeroinitializer

declare i32 @printf(ptr, ...)

define void @LC3_main() {
  %v = load i32, ptr @g               ; 42
  %v2 = add i32 %v, 1                 ; 43
  store i32 %v2, ptr @g

  %p = getelementptr [3 x i32], ptr @arr, i32 0, i32 1
  %e = load i32, ptr %p               ; 20

  %p2 = getelementptr i32, ptr @zarr, i32 2
  store i32 99, ptr %p2

  %sum = add i32 %v2, %e              ; 43 + 20 = 63
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %sum)
  ret void
}
