; ModuleID = 'example.c'
source_filename = "example.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-redhat-linux-gnu"

@.str = private unnamed_addr constant [4 x i8] c"-x:\00", align 1
@.str.1 = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@.str.2 = private unnamed_addr constant [3 x i8] c"y:\00", align 1

; Function Attrs: nounwind uwtable
define dso_local void @printInt(i32 noundef %x) local_unnamed_addr #0 {
entry:
  %cmp = icmp ugt i32 %x, 9
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  %div = udiv i32 %x, 10
  tail call void @printInt(i32 noundef %div)
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  %rem = urem i32 %x, 10
  %add = or disjoint i32 %rem, 48
  tail call void @printChar(i32 noundef %add) #4
  ret void
}

declare dso_local void @printChar(i32 noundef) local_unnamed_addr #1

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
define dso_local noundef i32 @f(i32 noundef %a, i32 noundef %b) local_unnamed_addr #2 {
entry:
  %0 = mul i32 %a, %b
  %mul = sub i32 0, %0
  ret i32 %mul
}

; Function Attrs: nofree nosync nounwind memory(none) uwtable
define dso_local range(i32 1, -2147483648) i32 @foo(i32 noundef %x) local_unnamed_addr #3 {
entry:
  br label %tailrecurse

tailrecurse:                                      ; preds = %cond.true, %entry
  %accumulator.tr = phi i32 [ 1, %entry ], [ %mul, %cond.true ]
  %x.tr = phi i32 [ %x, %entry ], [ %sub, %cond.true ]
  %cmp = icmp sgt i32 %x.tr, 0
  br i1 %cmp, label %cond.true, label %cond.end

cond.true:                                        ; preds = %tailrecurse
  %sub = add nsw i32 %x.tr, -1
  %mul = mul nuw nsw i32 %accumulator.tr, %x.tr
  br label %tailrecurse

cond.end:                                         ; preds = %tailrecurse
  %accumulator.ret.tr = mul nuw nsw i32 %accumulator.tr, 1
  ret i32 %accumulator.ret.tr
}

; Function Attrs: nounwind uwtable
define dso_local void @LC3_main() local_unnamed_addr #0 {
entry:
  %call1 = tail call i32 @foo(i32 noundef 7)
  tail call void @printStr(ptr noundef nonnull @.str) #4
  tail call void @printInt(i32 noundef 14)
  tail call void @printStr(ptr noundef nonnull @.str.1) #4
  tail call void @printStr(ptr noundef nonnull @.str.2) #4
  tail call void @printInt(i32 noundef %call1)
  tail call void @printStr(ptr noundef nonnull @.str.1) #4
  ret void
}

declare dso_local void @printStr(ptr noundef) local_unnamed_addr #1

attributes #0 = { nounwind uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { nofree nosync nounwind memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { nounwind }

!llvm.module.flags = !{!0, !1}
!llvm.ident = !{!2}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"uwtable", i32 2}
!2 = !{!"clang version 20.1.8 (Red Hat, Inc. 20.1.8-1.el10)"}
