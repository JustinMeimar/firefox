// AOT Baseline - empty stub for bootstrapping.
// When ENABLE_JS_AOT is set but no dump has been produced yet,
// this file provides the required symbols with zero-length sections.
// The runtime detects the empty container and falls back to normal codegen.

.section .text.aot,"ax",@progbits
.balign 4096
.global bl_aot_text_start
.global bl_aot_text_end
bl_aot_text_start:
.balign 4096
bl_aot_text_end:

.section .rodata
.balign 16
.global bl_aot_container_start
.global bl_aot_container_end
bl_aot_container_start:
bl_aot_container_end:

.section .note.GNU-stack,"",@progbits
