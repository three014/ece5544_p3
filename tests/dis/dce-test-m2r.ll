; ModuleID = 'dce-test-m2r.bc'
source_filename = "dce-test.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nofree norecurse nosync nounwind memory(none) uwtable
define dso_local i32 @foo(i32 noundef %a) local_unnamed_addr #0 {
entry:
  %add = add nsw i32 %a, 231
  %cmp.not42 = icmp sgt i32 %a, 0
  br label %start

start:                                            ; preds = %cleanup16, %entry
  %p.0 = phi i32 [ %add, %entry ], [ %p.3, %cleanup16 ]
  br i1 %cmp.not42, label %for.cond1.preheader, label %cleanup16

for.cond1.preheader:                              ; preds = %start, %cleanup
  %g.044 = phi i32 [ %add15, %cleanup ], [ 0, %start ]
  %p.143 = phi i32 [ %spec.select, %cleanup ], [ %p.0, %start ]
  br label %for.body4

for.body4:                                        ; preds = %for.cond1.preheader, %for.inc
  %j.037 = phi i32 [ %a, %for.cond1.preheader ], [ %dec, %for.inc ]
  %b.036 = phi i32 [ 0, %for.cond1.preheader ], [ %b.1, %for.inc ]
  %cmp5 = icmp eq i32 %j.037, 2
  br i1 %cmp5, label %for.inc, label %if.end

if.end:                                           ; preds = %for.body4
  %add6 = add nuw nsw i32 %j.037, %g.044
  %cmp7 = icmp eq i32 %add6, %a
  br i1 %cmp7, label %cleanup, label %for.inc

for.inc:                                          ; preds = %if.end, %for.body4
  %b.1 = phi i32 [ %b.036, %for.body4 ], [ %add6, %if.end ]
  %dec = add nsw i32 %j.037, -1
  %cmp2 = icmp slt i32 %j.037, 1
  br i1 %cmp2, label %cleanup, label %for.body4, !llvm.loop !5

cleanup:                                          ; preds = %for.inc, %if.end
  %cmp2.lcssa = phi i1 [ false, %if.end ], [ true, %for.inc ]
  %add11 = phi i32 [ 0, %if.end ], [ %b.1, %for.inc ]
  %spec.select = add nsw i32 %add11, %p.143
  %add15 = add nuw nsw i32 %g.044, 2
  %cmp.not = icmp slt i32 %add15, %a
  %or.cond = select i1 %cmp2.lcssa, i1 %cmp.not, i1 false
  br i1 %or.cond, label %for.cond1.preheader, label %cleanup16.loopexit, !llvm.loop !8

cleanup16.loopexit:                               ; preds = %cleanup
  %cmp.not.lcssa.ph = xor i1 %cmp2.lcssa, true
  br label %cleanup16

cleanup16:                                        ; preds = %cleanup16.loopexit, %start
  %cmp.not.lcssa = phi i1 [ false, %start ], [ %cmp.not.lcssa.ph, %cleanup16.loopexit ]
  %p.3 = phi i32 [ %p.0, %start ], [ %spec.select, %cleanup16.loopexit ]
  br i1 %cmp.not.lcssa, label %start, label %for.end18

for.end18:                                        ; preds = %cleanup16
  ret i32 %p.3
}

attributes #0 = { nofree norecurse nosync nounwind memory(none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 21.1.8 (https://github.com/llvm/llvm-project 2078da43e25a4623cab2d0d60decddf709aaea28)"}
!5 = distinct !{!5, !6, !7}
!6 = !{!"llvm.loop.mustprogress"}
!7 = !{!"llvm.loop.unroll.disable"}
!8 = distinct !{!8, !6, !7}
