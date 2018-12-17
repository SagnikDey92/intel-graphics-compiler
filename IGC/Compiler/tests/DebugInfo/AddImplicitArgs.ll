;===================== begin_copyright_notice ==================================

;Copyright (c) 2017 Intel Corporation

;Permission is hereby granted, free of charge, to any person obtaining a
;copy of this software and associated documentation files (the
;"Software"), to deal in the Software without restriction, including
;without limitation the rights to use, copy, modify, merge, publish,
;distribute, sublicense, and/or sell copies of the Software, and to
;permit persons to whom the Software is furnished to do so, subject to
;the following conditions:

;The above copyright notice and this permission notice shall be included
;in all copies or substantial portions of the Software.

;THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
;OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
;MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
;IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
;CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
;TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
;SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


;======================= end_copyright_notice ==================================
; RUN: igc_opt -igc-add-implicit-args -S %s -o - | FileCheck %s

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;; This LIT test checks that AddImplicitArgs pass handles variable debug info.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

target triple = "igil_32_GEN8"

define void @test(float addrspace(1)* %dst) #0 {
entry:
  call void @llvm.dbg.value(metadata float addrspace(1)* %dst, i64 0, metadata !9, metadata !DIExpression()), !dbg !12
  store float 1.000000e+00, float addrspace(1)* %dst, align 4
  ret void

; CHECK: define void @test(float addrspace(1)* %dst, <8 x i32> %r0, <8 x i32> %payloadHeader)
; CHECK: call void @llvm.dbg.value({{.*}})
}

declare void @llvm.dbg.value(metadata, i64, metadata, metadata) #1

attributes #0 = { alwaysinline nounwind }
attributes #1 = { nounwind readnone }

!igc.functions = !{!7}

!2 = !{!"function_type", i32 0}
!3 = !{i32 0}  ;; R0
!4 = !{i32 1}  ;; PAYLOAD_HEADER
!5 = !{!"implicit_arg_desc", !3, !4}
!6 = !{!2, !5}
!7 = !{void (float addrspace(1)*)* @test, !6}

!llvm.module.flags = !{!8}
!8 = !{i32 2, !"Debug Info Version", i32 3}

!10 = !DIBasicType(name: "float", size: 32, encoding: DW_ATE_float)
!11 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64)
!9 = !DILocalVariable(name: "", line: 144, scope: !13, file: !15, type: !11)
!12 = !DILocation(line: 0, column: 0, scope: !13)

!13 = distinct !DISubprogram(unit: !14)

!14 = distinct !DICompileUnit(language: DW_LANG_C, file: !15)

!15 = !DIFile(filename: "<stdin>", directory: "")

!llvm.dbg.cu = !{!14}
