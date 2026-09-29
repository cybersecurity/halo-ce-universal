OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(__guest_start)
PROVIDE(time = __time64);

PHDRS
{
  text PT_LOAD FLAGS(5);
  ro PT_LOAD FLAGS(4);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
  imports PT_LOAD FLAGS(5);
}

SECTIONS
{
  . = 0x00010000;
  .text : { *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :ro
  .preinit_array : { KEEP(*(.preinit_array*)) } :ro
  .init_array : {
    __guest_init_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __guest_init_end = .;
  } :ro
  .fini_array : {
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __fini_array_end = .;
  } :ro
  .data : { *(.data .data.*) } :data
  .tdata : { __tls_start = .; *(.tdata .tdata.*) } :data :tls
  .tbss (NOLOAD) : { *(.tbss .tbss.*) } :data :tls
  __tls_end = .;
  .bss (NOLOAD) : { *(.bss .bss.* COMMON) } :data
  . = ALIGN(0x1000);
  __guest_image_end = .;

  . = 0x70000000;
  .host_imports : { KEEP(*(.host_imports)) } :imports
}
