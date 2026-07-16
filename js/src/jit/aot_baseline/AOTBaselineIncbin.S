/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AOT Baseline embed wrapper. AOT_BASELINE_TEXT_BIN and
// AOT_BASELINE_CONTAINER_BIN are defined by aot_baseline/moz.build and
// point at build products written by DumpAOTContainer. Empty .bin files
// are valid: bl_aot_text_start == bl_aot_text_end causes the runtime to
// fall back to full codegen (see AOTContainerReader::fromEmbedded).

.section .text.aot,"ax",@progbits
.balign 4096
.global bl_aot_text_start
.global bl_aot_text_end
bl_aot_text_start:
.incbin AOT_BASELINE_TEXT_BIN
.balign 4096
bl_aot_text_end:

.section .rodata
.balign 16
.global bl_aot_container_start
.global bl_aot_container_end
bl_aot_container_start:
.incbin AOT_BASELINE_CONTAINER_BIN
bl_aot_container_end:

.section .note.GNU-stack,"",@progbits
