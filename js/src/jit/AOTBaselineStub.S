// AOT Baseline - empty stub for bootstrapping.
// When ENABLE_AOT_BASELINE is set but no dump has been produced yet,
// this file provides the required symbols with zero-length sections.
// The runtime detects the empty container and falls back to normal codegen.

.section .data
.balign 8
.global aot_tls_js_context_offset
aot_tls_js_context_offset:
    .quad 0

.section .text.aot,"ax",@progbits
.balign 16
.global bl_aot_text_start
.global bl_aot_text_end
bl_aot_text_start:
bl_aot_text_end:

.section .rodata
.balign 16
.global bl_aot_container_start
.global bl_aot_container_end
bl_aot_container_start:
bl_aot_container_end:

.section .note.GNU-stack,"",@progbits
