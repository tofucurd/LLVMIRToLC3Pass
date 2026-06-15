; Test printf with %d, %s, %c
@.fmt = private unnamed_addr constant [15 x i8] c"x=%d s=%s c=%c\00"
@.str = private unnamed_addr constant [6 x i8] c"hello\00"

declare void @printf(ptr, ...)

define void @LC3_main() {
  call void (ptr, ...) @printf(ptr @.fmt, i32 -42, ptr @.str, i32 65)
  ret void
}
