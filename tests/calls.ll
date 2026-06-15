@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define i32 @factorial(i32 %n) {
  %cmp = icmp sle i32 %n, 1
  br i1 %cmp, label %base, label %recurse

base:
  ret i32 1

recurse:
  %nm1 = sub i32 %n, 1
  %rec = call i32 @factorial(i32 %nm1)
  %result = mul i32 %n, %rec
  ret i32 %result
}

define void @LC3_main() {
  %x = call i32 @factorial(i32 5)  ; 120
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %x)
  ret void
}
