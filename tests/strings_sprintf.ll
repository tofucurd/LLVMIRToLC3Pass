; Test sprintf: copy strings into a buffer, then printf the buffer
@.world = private unnamed_addr constant [6 x i8] c"world\00"
@.fmt = private unnamed_addr constant [5 x i8] c"%s%c\00"
@.out = private unnamed_addr constant [3 x i8] c"%s\00"

declare void @sprintf(ptr, ptr, ...)
declare void @printf(ptr, ...)

define void @LC3_main() {
  %buf = alloca [64 x i32]
  call void (ptr, ptr, ...) @sprintf(ptr %buf, ptr @.fmt, ptr @.world, i32 33)
  call void (ptr, ...) @printf(ptr @.out, ptr %buf)
  ret void
}
