// AOT Baseline - empty stub for bootstrapping.
// When ENABLE_AOT_BASELINE is set but no dump has been produced yet,
// this file provides the required symbols with zero-length sections.
// The runtime detects the empty container and falls back to normal codegen.

.section .text.aot,"ax",@progbits
.balign 16
.global bl_aot_text_start
.global bl_aot_text_end

// JIT code can be written in here in .byte directives
// from a masm buffer. 
bl_aot_text_start:  
bl_aot_text_end:

.section .rodata
.balign 16
.global bl_aot_container_start
.global bl_aot_container_end

// Additional metadata needed to deserialize the AOT code
// can be organized here.
bl_aot_container_start:
bl_aot_container_end:

.section .note.GNU-stack,"",@progbits
