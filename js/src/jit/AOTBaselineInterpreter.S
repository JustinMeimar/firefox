// Stub AOT Baseline Interpreter (bootstrap build)
// Run --dump-bl to generate real data, then rebuild.
.section .rodata

.balign 16
.global bl_aot_code_start
.global bl_aot_code_end
bl_aot_code_start:
bl_aot_code_end:

.balign 4
.global bl_aot_InterpretOp
bl_aot_InterpretOp:    .long 0x00000000
.global bl_aot_InterpretOpNoDebugTrap
bl_aot_InterpretOpNoDebugTrap:    .long 0x00000000
.global bl_aot_BailoutPrologue
bl_aot_BailoutPrologue:    .long 0x00000000
.global bl_aot_ProfilerEnterToggle
bl_aot_ProfilerEnterToggle:    .long 0x00000000
.global bl_aot_ProfilerExitToggle
bl_aot_ProfilerExitToggle:    .long 0x00000000
.global bl_aot_DebugTrapHandler
bl_aot_DebugTrapHandler:    .long 0x00000000
.global bl_aot_DispatchTableOffset
bl_aot_DispatchTableOffset:    .long 0x00000000
.global bl_aot_CallVMDebugPrologue
bl_aot_CallVMDebugPrologue:    .long 0x00000000
.global bl_aot_CallVMDebugEpilogue
bl_aot_CallVMDebugEpilogue:    .long 0x00000000
.global bl_aot_CallVMDebugAfterYield
bl_aot_CallVMDebugAfterYield:    .long 0x00000000
.global bl_aot_HeaderSize
bl_aot_HeaderSize:    .long 0x00000000
.global bl_aot_PrologueEndOffset
bl_aot_PrologueEndOffset:    .long 0x00000000
.global bl_aot_DebugInstrumentationCount
bl_aot_DebugInstrumentationCount:    .long 0x00000000
.global bl_aot_DebugTrapCount
bl_aot_DebugTrapCount:    .long 0x00000000
.global bl_aot_CodeCoverageCount
bl_aot_CodeCoverageCount:    .long 0x00000000
.global bl_aot_ICReturnCount
bl_aot_ICReturnCount:    .long 0x00000000
.global bl_aot_RuntimePatchCount
bl_aot_RuntimePatchCount:    .long 0x00000000

.balign 4
.global bl_aot_DebugInstrumentationOffsets_start
.global bl_aot_DebugInstrumentationOffsets_end
bl_aot_DebugInstrumentationOffsets_start:
bl_aot_DebugInstrumentationOffsets_end:

.balign 4
.global bl_aot_DebugTrapOffsets_start
.global bl_aot_DebugTrapOffsets_end
bl_aot_DebugTrapOffsets_start:
bl_aot_DebugTrapOffsets_end:

.balign 4
.global bl_aot_CodeCoverageOffsets_start
.global bl_aot_CodeCoverageOffsets_end
bl_aot_CodeCoverageOffsets_start:
bl_aot_CodeCoverageOffsets_end:

.balign 4
.global bl_aot_ICReturnOffsets_start
.global bl_aot_ICReturnOffsets_end
bl_aot_ICReturnOffsets_start:
bl_aot_ICReturnOffsets_end:

.balign 4
.global bl_aot_RuntimePatches_start
.global bl_aot_RuntimePatches_end
bl_aot_RuntimePatches_start:
bl_aot_RuntimePatches_end:

.section .note.GNU-stack,"",@progbits
