PHDRS {
  aot_seg PT_LOAD FLAGS(5);
}

SECTIONS {
  .text.aot ALIGN(CONSTANT(MAXPAGESIZE)) : {
    KEEP(*(.text.aot))
  } :aot_seg
}

INSERT AFTER .text;
