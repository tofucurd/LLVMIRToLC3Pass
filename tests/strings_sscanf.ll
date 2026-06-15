; Test sscanf: parse integers from a buffer string
@.data = private unnamed_addr constant [8 x i8] c"99 42 \00\00"
@.fmt_in = private unnamed_addr constant [6 x i8] c"%d %d\00"
@.fmt_out = private unnamed_addr constant [7 x i8] c"%d,%d \00"

declare void @sscanf(ptr, ptr, ...)
declare void @printf(ptr, ...)

define void @LC3_main() {
  %a = alloca i32
  %b = alloca i32
  call void (ptr, ptr, ...) @sscanf(ptr @.data, ptr @.fmt_in, ptr %a, ptr %b)
  %av = load i32, ptr %a
  %bv = load i32, ptr %b
  call void (ptr, ...) @printf(ptr @.fmt_out, i32 %av, i32 %bv)
  ret void
}
