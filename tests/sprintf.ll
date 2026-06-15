; Test sprintf with %d, %s, %c
@.fmt = private unnamed_addr constant [15 x i8] c"x=%d s=%s c=%c\00"
@.str = private unnamed_addr constant [6 x i8] c"world\00"

declare void @sprintf(ptr, ptr, ...)

define void @LC3_main() {
  %buf = alloca [64 x i32]
  call void (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.fmt, i32 123, ptr @.str, i32 88)
  ret void
}
