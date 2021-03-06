; RUN: llc < %s -mtriple=ve-unknown-unknown | FileCheck %s

; Function Attrs: nounwind
define void @pvsrl_vvs(i32* %pvx, i32* %pvz, i64 %sy, i32 %n) {
; CHECK-LABEL: pvsrl_vvs
; CHECK: .LBB0_2
; CHECK: 	pvsrl %v0,%v0,%s2
entry:
  %cmp16 = icmp sgt i32 %n, 0
  br i1 %cmp16, label %for.body, label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.body, %entry
  ret void

for.body:                                         ; preds = %entry, %for.body
  %pvx.addr.019 = phi i32* [ %add.ptr, %for.body ], [ %pvx, %entry ]
  %pvz.addr.018 = phi i32* [ %add.ptr4, %for.body ], [ %pvz, %entry ]
  %i.017 = phi i32 [ %add, %for.body ], [ 0, %entry ]
  %sub = sub nsw i32 %n, %i.017
  %cmp1 = icmp slt i32 %sub, 512
  %0 = ashr i32 %sub, 1
  %conv3 = select i1 %cmp1, i32 %0, i32 256
  tail call void @llvm.ve.lvl(i32 %conv3)
  %1 = bitcast i32* %pvz.addr.018 to i8*
  %2 = tail call <256 x double> @llvm.ve.vld.vss(i64 8, i8* %1)
  %3 = tail call <256 x double> @llvm.ve.pvsrl.vvs(<256 x double> %2, i64 %sy)
  %4 = bitcast i32* %pvx.addr.019 to i8*
  tail call void @llvm.ve.vst.vss(<256 x double> %3, i64 8, i8* %4)
  %add.ptr = getelementptr inbounds i32, i32* %pvx.addr.019, i64 512
  %add.ptr4 = getelementptr inbounds i32, i32* %pvz.addr.018, i64 512
  %add = add nuw nsw i32 %i.017, 512
  %cmp = icmp slt i32 %add, %n
  br i1 %cmp, label %for.body, label %for.cond.cleanup
}

; Function Attrs: nounwind
declare void @llvm.ve.lvl(i32)

; Function Attrs: nounwind readonly
declare <256 x double> @llvm.ve.vld.vss(i64, i8*)

; Function Attrs: nounwind readnone
declare <256 x double> @llvm.ve.pvsrl.vvs(<256 x double>, i64)

; Function Attrs: nounwind writeonly
declare void @llvm.ve.vst.vss(<256 x double>, i64, i8*)

