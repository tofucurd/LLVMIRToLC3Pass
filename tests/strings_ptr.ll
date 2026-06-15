; Test printing string pointers via printf %s
@.hello = private unnamed_addr constant [6 x i8] c"hello\00"
@.fmt = private unnamed_addr constant [3 x i8] c"%s\00"

declare void @printf(ptr, ...)

define void @LC3_main() {
  call void (ptr, ...) @printf(ptr @.fmt, ptr @.hello)
  call void (ptr, ...) @printf(ptr @.fmt, ptr @.hello)
  ret void
}
