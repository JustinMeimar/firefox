/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Embeds the generated AOT image and exports symbols for its byte range. When
 * no recorded image is available, the build embeds an empty valid image so
 * runtime generation remains available.
 */

    .section .text.aot,"ax",@progbits
    .balign 4096
    .globl  aot_image_start
    .type   aot_image_start, @object
aot_image_start:
    .incbin "AOTImage.inc"
    .balign 4096
    .globl  aot_image_end
    .type   aot_image_end, @object
aot_image_end:
    .size   aot_image_start, aot_image_end - aot_image_start

    .section .note.GNU-stack,"",@progbits
