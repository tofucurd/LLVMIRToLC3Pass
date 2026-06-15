; Test scanf with %d, %s, %c
@.fmt = private unnamed_addr constant [9 x i8] c"%d %c %s\00"

declare void @scanf(ptr, ...)

define void @LC3_main() {
  %x = alloca i32
  %ch = alloca i32
  %buf = alloca [32 x i32]
  call void (ptr, ...) @scanf(ptr @.fmt, ptr %x, ptr %ch, ptr %buf)
  ret void
}
