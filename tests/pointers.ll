@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @set(ptr %dst, i32 %val) {
  store i32 %val, ptr %dst
  ret void
}

define i32 @get(ptr %src) {
  %v = load i32, ptr %src
  ret i32 %v
}

define void @LC3_main() {
  %x = alloca i32
  store i32 5, ptr %x

  %p = alloca i32
  store ptr %x, ptr %p              ; p = &x
  %p_val = load ptr, ptr %p
  store i32 10, ptr %p_val          ; *p = 10

  call void @set(ptr %x, i32 99)     ; *x = 99
  %v = call i32 @get(ptr %x)         ; 99
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %v)
  ret void
}
