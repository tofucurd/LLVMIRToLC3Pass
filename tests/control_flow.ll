@.fmt = private unnamed_addr constant [4 x i8] c"%d \00"

declare i32 @printf(ptr, ...)

define void @LC3_main() {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop_body ]
  %sum = phi i32 [ 0, %entry ], [ %sum_next, %loop_body ]
  %cmp = icmp slt i32 %i, 5
  br i1 %cmp, label %loop_body, label %exit

loop_body:
  %next = add i32 %i, 1
  %sum_next = add i32 %sum, %i  ; 0+1+2+3+4 = 10
  br label %loop

exit:
  %ucmp = icmp ult i32 100, 40000
  %sel = select i1 %ucmp, i32 1, i32 0
  %result = add i32 %sum, %sel  ; 10 + 1 = 11
  call i32 (ptr, ...) @printf(ptr @.fmt, i32 %result)
  ret void
}
